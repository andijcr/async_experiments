export module est:util.scope_exit;

import std;

export namespace est {

// Runs `fn` when the guard goes out of scope, however it exits (normal
// return or exception) - a generic version of the ad-hoc RAII guards
// this codebase kept hand-rolling for exactly this purpose (e.g.
// est::future's shared_state::run(), before this existed).
template <std::invocable Fn> class scope_exit {
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
