export module larson_scanner_app;

import est;
import larson_scanner;
import std;

// The est::-dependent half of this example, split out of main.cpp:
// everything that talks to est::loop/est::schedule_periodic/
// est::spsc_ring<T>/est::external_event<T> - and the coroutine
// (drain_commands(), below) that ties them together - lives here now,
// mirroring examples/spreadsheet/'s own spreadsheet.cppm/spreadsheet_io.cppm
// split (a pure-logic module plus a thin est-aware one). larson_scanner.cppm
// itself stays exactly as free of est:: as before.
//
// app's own public surface is deliberately narrower than the machinery
// behind it: push_command()/loop() are plain, blocking function calls, and
// render_callback is handed a led_buffer, not a string - no est::future<T>
// anywhere in this module's exported interface, and no UTF8/ANSI
// rendering-to-text logic either. That stays where it already lived, in
// larson_scanner::render() (larson_scanner.cppm) - main.cpp calls it from
// inside the render_callback it hands to app's constructor, the same
// division of labor spreadsheet_io.cppm's getline_async() has from
// spreadsheet.cppm's own parse_line()/execute().
export namespace larson_scanner {

// Everything app's constructor needs, gathered into one aggregate rather
// than a long positional parameter list - the same reasoning
// est::spsc_ring<T>'s own capacity argument or led_buffer's constructor
// would otherwise multiply into, times three (width/speed/decay) plus
// three more (the periodic intervals) plus one (queue capacity) as
// distinct positional floats/durations, an easy-to-transpose shape
// bugprone-easily-swappable-parameters exists to flag.
struct app_config {
  std::size_t width = 40;
  float initial_speed = 20.0F;
  float initial_decay = 0.01F;
  std::chrono::milliseconds tick_interval{20};
  std::chrono::milliseconds render_interval{33};
  std::chrono::milliseconds command_poll_interval{20};
  std::size_t command_queue_capacity = 16;
};

// Owns an est::loop and every piece of wiring around it - the physics-tick,
// render-trigger, and command-poll periodic timers; the
// est::spsc_ring<command>/est::external_event<std::size_t> handoff; the
// drain_commands() coroutine that reacts to it - behind an interface with
// no coroutines/futures/timers visible to a caller at all.
class app {
public:
  // Invoked on the loop thread, once per render_interval, with the
  // current led_buffer - this class has no opinion on how (or whether)
  // that becomes text; it only decides *when*.
  using render_callback = std::function<void(const led_buffer&)>;

  explicit app(app_config config, render_callback on_render)
      : buffer_(config.width, config.initial_speed, config.initial_decay),
        on_render_(std::move(on_render)), tick_interval_(config.tick_interval),
        render_interval_(config.render_interval), poll_interval_(config.command_poll_interval),
        commands_(config.command_queue_capacity) {}

  app(const app&) = delete;
  auto operator=(const app&) -> app& = delete;
  app(app&&) = delete;
  auto operator=(app&&) -> app& = delete;
  ~app() = default;

  // External-context only (never called concurrently with itself) -
  // typically a dedicated input thread, same precondition
  // est::spsc_ring<T>::try_push() itself already carries. Spins
  // (std::this_thread::yield()) until the ring accepts cmd rather than
  // dropping it - this data matters, matching spsc_ring<T>'s own
  // reject-on-full-not-overwrite posture. A CommandKind::quit command is
  // not special-cased here; loop()'s own drain_commands() is what
  // recognizes one and stops the loop, the same as every other command
  // kind is dispatched.
  //
  // EST_NO_THREADS (cmake/toolchain-mps2an385.cmake's own comment): a
  // genuinely single-core, no-OS-threads target has nothing for
  // yield() to hand off to - std::this_thread doesn't even exist there
  // (freestanding libc++, no <thread>) - so the backoff hint itself is
  // skipped, leaving a plain busy-spin (a caller on such a target only
  // ever calls this from the loop thread itself, never contending with
  // another thread for the ring buffer at all).
  void push_command(command cmd) noexcept {
    // A fresh copy each attempt, not std::move(cmd) - command is
    // trivially copyable, so this costs nothing extra, and it sidesteps
    // clang-tidy's own (here overly conservative) bugprone-use-after-move:
    // a rejected try_push() never actually moves from its argument at all
    // (see est::spsc_ring<T>::try_push()'s own doc comment), but that's
    // not something the check can see across loop iterations.
    while (!commands_.try_push(command{cmd})) {
#ifndef EST_NO_THREADS
      std::this_thread::yield();
#endif
    }
    command_count_.store(++pushed_, std::memory_order_release);
  }

  // Runs until a quit command is processed. Blocks the calling thread -
  // registers this app's own loop_ as est::current_loop() for the
  // duration, starts the three periodic timers and the drain coroutine,
  // then calls loop_.run(). No future<T> in this function's own
  // signature; drain_commands()'s coroutine frame is this call's own
  // local variable, never observed by a caller.
  void loop() {
    const auto loop_guard = est::make_current_loop(loop_);
    auto tick_handle =
        est::schedule_periodic(tick_interval_, [this] { buffer_.tick(tick_interval_); });
    auto render_handle = est::schedule_periodic(render_interval_, [this] { on_render_(buffer_); });
    auto poll_handle = est::schedule_periodic(poll_interval_, [this] { new_commands_.poll(); });
    auto commands_task = drain_commands();
    loop_.run();
  }

private:
  // Loop-thread only. Suspends on new_commands_.wait() between batches -
  // woken only once the poll_handle timer above has actually observed
  // command_count_ move, never on a fixed schedule of its own. Private:
  // this is app's own implementation detail, started once by loop()
  // above and never named by a caller.
  [[nodiscard]] auto drain_commands() -> est::future<void> {
    for (;;) {
      co_await new_commands_.wait();
      new_commands_.reset();
      while (const auto cmd = commands_.try_pop()) {
        if (cmd->kind == CommandKind::quit) {
          loop_.stop();
          co_return;
        }
        apply(buffer_, *cmd);
      }
    }
  }

  led_buffer buffer_;
  render_callback on_render_;
  std::chrono::milliseconds tick_interval_;
  std::chrono::milliseconds render_interval_;
  std::chrono::milliseconds poll_interval_;

  est::loop loop_;
  est::spsc_ring<command> commands_;
  // Written by push_command() (any thread), read by new_commands_ below
  // (loop thread only, inside poll()) - the one deliberate cross-thread
  // seam, same shape as every other est::external_event<T> use in this
  // codebase.
  std::atomic<std::size_t> command_count_{0};
  est::external_event<std::size_t> new_commands_{command_count_};
  // External-context only, same as push_command() itself - a plain
  // (non-atomic) running count of pushes so far, not touched by anything
  // on the loop thread.
  std::size_t pushed_ = 0;
};

} // namespace larson_scanner
