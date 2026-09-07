// Deliberately does *not* install, or even know about, a concrete
// platform::interface - per review, est itself should have zero trace of
// any specific backend, not even a partition re-exported for convenience.
// `hosted_stdcpp` (the one backend that exists) lives in `estext`, a
// genuinely separate module (`estext/src/hosted_stdcpp.cppm`) rather than
// one of est's own partitions - `import est;` alone gives a consumer the
// complete framework with nothing hosted-OS-specific anywhere in it; a
// consumer that wants a working, ready-to-use backend opts in with a
// second import, `import estext;`, then constructs one and installs it
// via `est::platform::override_instance()`, keeping the returned guard
// alive for as long as the program needs a platform installed (typically
// for all of `main()`). `examples/hello_world/main.cpp` and
// `examples/sleep_sort/main.cpp` both do exactly this; `est/tests/`'s own
// Catch2 binary does it once via a custom `main()`
// (est/tests/test_main.cpp), rather than in every individual test file.
export module est;

export import :util.intrusive_list;
export import :util.scope_exit;
export import :util.shared_ptr;
export import :platform;
export import :check;
export import :sync.mutex;
export import :timer;
export import :loop;
export import :util.current_loop;
export import :future;
export import :promise;
