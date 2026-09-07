export module est;

export import :util.intrusive_list;
export import :util.scope_exit;
export import :util.shared_ptr;
export import :platform;
export import :platform.hosted_stdcpp;
export import :check;
export import :sync.mutex;
export import :timer;
export import :loop;
export import :future;
export import :promise;

// est::platform can't construct a default backend itself (:platform's own
// top comment on why hosted_stdcpp lives in its own module) - this is
// where that actually happens: a process-lifetime hosted_stdcpp,
// installed as the permanent default the moment `import est;` runs
// anywhere, via the exact same override_instance() every test already
// uses to install a *temporary* one. Storing its returned guard forever
// (rather than letting it go out of scope) is what makes the install
// permanent instead of scoped - nothing ever restores `nullptr` over it.
namespace est::detail {
// False positive below: bugprone-throwing-static-initialization flags
// hosted_stdcpp's implicit default constructor as "possibly throwing"
// purely because it has non-static data members of its own
// (:platform.hosted_stdcpp) - it doesn't actually analyze whether those
// members' own default construction can throw: a
// std::chrono::steady_clock::time_point, a void*, and a
// default-constructed (disengaged) std::optional<loop> all trivially/
// noexcept default-construct, it just treats "not a literally empty
// class" as enough to warn. Confirmed by testing: any non-static member
// on a type constructed this way triggers the identical warning,
// regardless of type.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
inline platform::hosted_stdcpp default_platform_instance{};
inline auto default_platform_guard = platform::override_instance(default_platform_instance);
} // namespace est::detail
