// Deliberately does not install, or even know about, a concrete
// platform::interface - est has zero trace of any specific backend, not
// even a partition re-exported for convenience. `hosted_stdcpp` (the one
// backend that exists) lives in `estext`, a genuinely separate module
// (`estext/src/hosted_stdcpp.cppm`). A consumer that wants a working
// backend opts in with a second import, `import estext;`, constructs
// one, and installs it via `est::platform::override_instance()`, keeping
// the returned guard alive for as long as the program needs a platform
// installed (typically all of `main()`) - see
// `examples/hello_world/main.cpp` or `est/tests/test_main.cpp`.
export module est;

export import :util.intrusive_list;
export import :util.scope_exit;
export import :util.shared_ptr;
export import :platform;
export import :check;
export import :util.jitter;
export import :sync.mutex;
export import :sync.event;
export import :sync.external_event;
export import :sync.spsc_ring;
export import :timer;
export import :loop;
export import :util.current_loop;
export import :timer.periodic;
export import :future;
export import :promise;
export import :when_all;
export import :when_any;
export import :when_any_succeeds;
