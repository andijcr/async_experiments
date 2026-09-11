export module est:util.scope_exit;

import std;

export namespace est {

// Runs `fn` when the guard goes out of scope, however it exits (normal
// return or exception).
//
// Fn must be nothrow-invocable: ~scope_exit() calls it unconditionally,
// including while another exception is already propagating (the guard
// exiting via an exception is exactly the case this class exists for) -
// a throwing fn_ there would call std::terminate, and even without an
// active exception, a throwing destructor is its own hazard. Constrained
// here, at the type, rather than left as an unstated precondition.
template <class Fn>
  requires std::is_nothrow_invocable_v<Fn>
class scope_exit {
public:
  explicit scope_exit(Fn fn) noexcept(std::is_nothrow_move_constructible_v<Fn>)
      : fn_(std::move(fn)) {}
  scope_exit(const scope_exit&) = delete;
  auto operator=(const scope_exit&) -> scope_exit& = delete;
  scope_exit(scope_exit&&) = delete;
  auto operator=(scope_exit&&) -> scope_exit& = delete;
  ~scope_exit() { fn_(); }

private:
  Fn fn_;
};

} // namespace est
