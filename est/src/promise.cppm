export module est:promise;

import std;
import :future;
import :util.shared_ptr;

export namespace est {

// Producer handle: a thin, move-only view over a future_state<T>.
template <class T> class promise {
public:
  explicit promise(shared_ptr<future_state<T>> state) noexcept : state_(std::move(state)) {}
  promise(const promise&) = delete;
  auto operator=(const promise&) -> promise& = delete;
  promise(promise&&) noexcept = default;
  auto operator=(promise&&) noexcept -> promise& = default;
  ~promise() = default;

  void set_value(const T& value) { state_->set_value(value); }
  void set_value(T&& value) { state_->set_value(std::move(value)); }
  void set_exception(std::exception_ptr exception) { state_->set_exception(std::move(exception)); }

private:
  shared_ptr<future_state<T>> state_;
};

// Constructs a fresh future_state<T> (via `allocator`, defaulting to the
// process-wide default std::pmr resource) and returns the promise/future
// pair that share it. This is the only way a future_state gets created -
// promise<T>/future<T> only otherwise exist as the result of a move.
template <class T>
auto make_promise_future(std::pmr::polymorphic_allocator<std::byte> allocator = {})
    -> std::pair<promise<T>, future<T>> {
  auto state = shared_ptr<future_state<T>>::make(allocator, allocator);
  auto state_for_future = state; // copy bumps the ref count from 1 to 2
  return {promise<T>(std::move(state)), future<T>(std::move(state_for_future))};
}

} // namespace est
