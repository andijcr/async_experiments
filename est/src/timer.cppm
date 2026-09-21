export module est:timer;

import std;
import :platform;

export namespace est {

// A min-heap of pending one-shot deadlines, ordered by
// platform::instance().now(). Templated on the allocator used for the
// underlying storage. Just the scheduling structure - est::loop owns one
// and is what actually fires deadlines as continuations.
template <class Allocator = std::allocator<std::byte>> class timer_queue {
public:
  using clock = platform::clock;
  using time_point = clock::time_point;
  using duration = clock::duration;
  using id = std::size_t;

  explicit timer_queue(const Allocator& allocator = Allocator()) : entries_(allocator) {}

  // Schedules a one-shot deadline; returns an id usable with cancel().
  auto schedule_at(time_point deadline) -> id {
    const id new_id = next_id_++;
    entries_.push_back(entry{.deadline = deadline, .timer_id = new_id});
    std::ranges::push_heap(entries_, std::ranges::greater{}, &entry::deadline);
    return new_id;
  }

  auto schedule_after(duration delay) -> id {
    return schedule_at(platform::instance().now() + delay);
  }

  // Returns false if `target` wasn't found (already fired, or invalid).
  auto cancel(id target) -> bool {
    const auto it = std::ranges::find(entries_, target, &entry::timer_id);
    if (it == entries_.end()) {
      return false;
    }
    entries_.erase(it);
    std::ranges::make_heap(entries_, std::ranges::greater{}, &entry::deadline);
    return true;
  }

  [[nodiscard]] auto empty() const noexcept -> bool { return entries_.empty(); }

  [[nodiscard]] auto next_deadline() const -> std::optional<time_point> {
    if (entries_.empty()) {
      return std::nullopt;
    }
    return entries_.front().deadline;
  }

  // Pops and returns the earliest entry's id if its deadline is not
  // after `now`; nullopt otherwise (nothing ready yet).
  auto pop_ready(time_point now) -> std::optional<id> {
    if (entries_.empty() || entries_.front().deadline > now) {
      return std::nullopt;
    }
    std::ranges::pop_heap(entries_, std::ranges::greater{}, &entry::deadline);
    const id ready_id = entries_.back().timer_id;
    entries_.pop_back();
    return ready_id;
  }

private:
  // Always fully constructed via a designated initializer at its one use
  // site (schedule_at()) - never default-constructed - so `timer_id`
  // needs no default member initializer to actually be safe. Suppressed
  // rather than given a `= 0` that would misleadingly suggest a
  // meaningful default id for a case that can't occur.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
  struct entry {
    time_point deadline;
    id timer_id;
  };

  using entry_allocator = std::allocator_traits<Allocator>::template rebind_alloc<entry>;
  std::vector<entry, entry_allocator> entries_;
  id next_id_ = 0;
};

} // namespace est
