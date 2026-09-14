export module est:sync.spsc_ring;

import std;
import :check;

export namespace est {

// A fixed-capacity, single-producer/single-consumer ring buffer - no
// mutex, coordinated through two atomic indices instead of one, each
// written by exactly one side and only ever read by the other.
// try_push() is external-context only (the producer - another thread, an
// ISR, a future bare-metal interrupt handler); try_pop() is loop-context
// only (the consumer). The same "the atomic is the one deliberate FFI
// boundary, everything else stays single-threaded" shape
// est::external_event<T> (est:sync.external_event) already established
// for a single value, generalized here to a bounded stream of them -
// unlike external_event<T>, which collapses several writes between two
// poll() calls into "the latest value," every value successfully pushed
// here is delivered exactly once, in order, or not at all (rejected
// while full).
//
// Capacity is a runtime constructor parameter, not a compile-time `N`,
// backed by one allocation from the caller's own
// std::pmr::polymorphic_allocator<std::byte> - matching this codebase's
// allocator-first stance (docs/wiki/Allocation-Patterns.md) rather than
// a fixed-size embedded array.
//
// T must be nothrow-movable and default-constructible: try_push()/
// try_pop() move T into and out of buffer_ rather than copy it, so a
// move-only "slot" type - most usefully est::spsc_ring<std::unique_ptr<U>>,
// handing off ownership of a heap-allocated item per slot - works
// alongside plain PODs (an int, a small struct, a std::chrono duration).
// The moves must be noexcept: try_push()/try_pop() are themselves
// noexcept, and a throwing move out of buffer_ (leaving the slot's state
// ambiguous mid-handoff) is exactly the hazard that promise is meant to
// rule out. A trivially copyable T satisfies this trivially - its "move"
// is the same non-throwing copy it always was - so this is a strict
// relaxation of the class's original trivially-copyable-only constraint,
// not a different shape.
//
// **Reject-on-full, not overwrite-on-full**: try_push() returns false
// once the ring is full rather than silently dropping the oldest queued
// value - the safer default for "this data actually matters" (log
// records, incoming frames, queued commands, the motivating examples).
// A caller that explicitly wants "keep only the latest N" (a metrics/
// telemetry use case) can drain more aggressively instead; this class
// doesn't guess which policy a given caller wants.
template <class T>
  requires std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T> &&
           std::default_initializable<T>
class spsc_ring {
public:
  using allocator_type = std::pmr::polymorphic_allocator<std::byte>;

  // capacity must be positive - checked. One extra slot is always
  // allocated beyond it (buffer_.size() == capacity + 1): the classic
  // "one slot permanently unused" trick for telling full apart from
  // empty with nothing more than the two indices already needed anyway -
  // full is write_index_ + 1 (mod buffer_.size()) == read_index_, empty
  // is write_index_ == read_index_, and neither needs a separate atomic
  // count or a generation/wrap bit alongside them.
  explicit spsc_ring(std::size_t capacity, allocator_type allocator = {})
      : capacity_(capacity), buffer_(capacity + 1, allocator) {
    check(capacity > 0, "est::spsc_ring: capacity must be positive");
  }

  spsc_ring(const spsc_ring&) = delete;
  auto operator=(const spsc_ring&) -> spsc_ring& = delete;
  spsc_ring(spsc_ring&&) = delete;
  auto operator=(spsc_ring&&) -> spsc_ring& = delete;
  ~spsc_ring() = default;

  [[nodiscard]] auto capacity() const noexcept -> std::size_t { return capacity_; }

  // External-context (producer) only - never called concurrently with
  // itself, only ever racing try_pop() on the other side. Returns false,
  // leaving `value` unconsumed, if the ring is already full.
  //
  // read_index_ is loaded with memory_order_acquire, pairing with
  // try_pop()'s own memory_order_release store to it below - not because
  // this call reads anything *through* that index (it only compares it),
  // but so a full ring observed here reflects every slot try_pop() has
  // actually finished reading, never a stale view that would reject a
  // push the consumer already made room for.
  //
  // Takes T&&, not T by value: the full check below has to run *before*
  // value is touched at all, not just before it's written into buffer_.
  // A by-value parameter would already have moved-from the caller's
  // object at the call site (`try_push(std::move(x))` moves into the
  // parameter unconditionally), so a rejected push of a move-only T would
  // silently destroy it with no way to hand it back - exactly the "this
  // data actually matters" guarantee this class exists to uphold. Binding
  // by reference instead defers the move to the one call to std::move()
  // below, which only runs once the ring is known to have room.
  //
  // The write into buffer_[write_index_] is a plain, unsynchronized move
  // assignment - safe because slot write_index_ is never touched by
  // try_pop() until the release store just below publishes it as
  // readable, and no other producer call can be racing this one (single
  // producer). That release store is what actually hands the just-written
  // slot off to the consumer: it publishes both the new index and,
  // via the acquire load try_pop() pairs it with, the value just written
  // into that slot.
  [[nodiscard]] auto try_push(T&& value) noexcept -> bool {
    const auto write_index = write_index_.load(std::memory_order_relaxed);
    const auto next_write = advance(write_index);
    if (next_write == read_index_.load(std::memory_order_acquire)) {
      return false; // full - value untouched, still owned by the caller
    }
    // write_index is always < buffer_.size(): it was either the initial
    // 0 or a prior advance() result, and advance() itself never returns
    // a value >= buffer_.size(). A bounds-checked .at() here would only
    // ever either pass silently or throw std::out_of_range straight
    // through this noexcept function into std::terminate() - strictly
    // worse than the plain access below, not safer, for a case that
    // provably cannot occur.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    buffer_[write_index] = std::move(value);
    write_index_.store(next_write, std::memory_order_release);
    return true;
  }

  // Loop-context (consumer) only - never called concurrently with
  // itself, only ever racing try_push() on the other side. Returns
  // nullopt if the ring is empty.
  //
  // write_index_ is loaded with memory_order_acquire, pairing with
  // try_push()'s own memory_order_release store above - this is what
  // makes the value try_push() wrote into buffer_[read_index_] actually
  // visible here, not just the index that names it.
  //
  // read_index_'s own store below is memory_order_release for the
  // identical reason try_push()'s write_index_ store is: it publishes
  // both the freed slot and (paired with try_push()'s own acquire load
  // of read_index_) everything this call already did with the value it
  // just read out, before the producer can consider that slot reusable.
  [[nodiscard]] auto try_pop() noexcept -> std::optional<T> {
    const auto read_index = read_index_.load(std::memory_order_relaxed);
    if (read_index == write_index_.load(std::memory_order_acquire)) {
      return std::nullopt; // empty
    }
    // read_index is always < buffer_.size(), for the identical reason
    // try_push()'s own write_index access above is - see its comment.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    std::optional<T> value{std::move(buffer_[read_index])};
    read_index_.store(advance(read_index), std::memory_order_release);
    return value;
  }

private:
  [[nodiscard]] auto advance(std::size_t index) const noexcept -> std::size_t {
    const auto next = index + 1;
    return next == buffer_.size() ? 0 : next;
  }

  std::size_t capacity_;
  std::pmr::vector<T> buffer_;
  std::atomic<std::size_t> write_index_{0};
  std::atomic<std::size_t> read_index_{0};
};

} // namespace est
