module;

#include <exception>
#include <memory_resource>
#include <utility>

export module est:promise;

import :future;

export namespace est {

// Producer handle: a thin, move-only view over a shared_state<T>.
template <class T> class promise {
public:
  explicit promise(shared_state<T>* state) noexcept : state_(state) {}
  promise(const promise&) = delete;
  auto operator=(const promise&) -> promise& = delete;

  promise(promise&& other) noexcept : state_(std::exchange(other.state_, nullptr)) {}

  auto operator=(promise&& other) noexcept -> promise& {
    std::swap(state_, other.state_);
    return *this;
  }

  ~promise() { reset(); }

  void set_value(const T& value) { state_->set_value(value); }
  void set_value(T&& value) { state_->set_value(std::move(value)); }
  void set_exception(std::exception_ptr exception) { state_->set_exception(std::move(exception)); }

private:
  void reset() noexcept {
    if (state_ != nullptr) {
      state_->release();
      state_ = nullptr;
    }
  }

  shared_state<T>* state_ = nullptr;
};

// Constructs a fresh shared_state<T> (via `allocator`, defaulting to the
// process-wide default std::pmr resource) and returns the promise/future
// pair that share it. This is the only way a shared_state gets created -
// promise<T>/future<T> only otherwise exist as the result of a move.
template <class T>
auto make_promise_future(std::pmr::polymorphic_allocator<std::byte> allocator = {})
    -> std::pair<promise<T>, future<T>> {
  auto* state = allocator.template new_object<shared_state<T>>(allocator);
  state->add_ref();
  state->add_ref();
  return {promise<T>(state), future<T>(state)};
}

} // namespace est
