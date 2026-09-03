module;

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

export module est:timer;

import :platform;

export namespace est {

// A min-heap of pending one-shot deadlines, ordered by Platform::now().
// Templated on the allocator used for the underlying storage, part of
// the framework's allocator-first design (docs/PLAN.md, "Allocator
// support") rather than bolted on later. Periodic timers and actually
// firing continuations are est::loop's job (M3); this is just the
// scheduling structure a future loop will own and drive.
template <class Platform = platform::hosted_linux, class Allocator = std::allocator<std::byte>>
class timer_queue {
public:
  using clock = typename Platform::clock;
  using time_point = typename Platform::time_point;
  using duration = typename Platform::duration;
  using id = std::size_t;

  explicit timer_queue(const Allocator& allocator = Allocator()) : entries_(allocator) {}

  // Schedules a one-shot deadline; returns an id usable with cancel().
  auto schedule_at(time_point deadline) -> id {
    const id new_id = next_id_++;
    entries_.push_back(entry{.deadline = deadline, .timer_id = new_id});
    std::push_heap(entries_.begin(), entries_.end(), by_deadline_descending);
    return new_id;
  }

  auto schedule_after(duration delay) -> id { return schedule_at(Platform::now() + delay); }

  // Returns false if `target` wasn't found (already fired, or invalid).
  auto cancel(id target) -> bool {
    const auto it = std::find_if(entries_.begin(), entries_.end(), [target](const entry& e) {
      return e.timer_id == target;
    });
    if (it == entries_.end()) {
      return false;
    }
    entries_.erase(it);
    std::make_heap(entries_.begin(), entries_.end(), by_deadline_descending);
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
    std::pop_heap(entries_.begin(), entries_.end(), by_deadline_descending);
    const id ready_id = entries_.back().timer_id;
    entries_.pop_back();
    return ready_id;
  }

private:
  struct entry {
    time_point deadline;
    id timer_id;
  };

  // std::push_heap/pop_heap build a max-heap by default; negating the
  // comparison surfaces the *earliest* deadline at entries_.front().
  static auto by_deadline_descending(const entry& lhs, const entry& rhs) -> bool {
    return lhs.deadline > rhs.deadline;
  }

  using entry_allocator = typename std::allocator_traits<Allocator>::template rebind_alloc<entry>;
  std::vector<entry, entry_allocator> entries_;
  id next_id_ = 0;
};

} // namespace est
