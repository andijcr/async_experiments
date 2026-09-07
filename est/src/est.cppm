// Deliberately does *not* install a default platform::interface as a side
// effect of `import est;` - per review, that's the entry point's own
// responsibility (a program's `main()`, or a test binary's own setup),
// not something this library should do invisibly. See
// est::platform::hosted_stdcpp's own doc comment (:platform.hosted_stdcpp)
// for how a consumer installs one: construct it, then
// `est::platform::override_instance(instance)`, keeping the returned
// guard alive for as long as the program needs a platform installed
// (typically for all of `main()`). `examples/hello_world/main.cpp` and
// `examples/sleep_sort/main.cpp` both do exactly this; `est/tests/`'s own
// Catch2 binary does it once via a custom `main()`
// (est/tests/test_main.cpp), rather than in every individual test file.
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
