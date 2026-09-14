import est;
import estext;
import larson_scanner;
import std;

// EXIT_SUCCESS/EXIT_FAILURE and std::setvbuf()'s _IOLBF are macros/
// plain declarations `import std;` doesn't carry (same reasoning as
// examples/spreadsheet/main.cpp), so these stay classic #includes.
#include <cstdio>
#include <cstdlib>

namespace {

// Drains every command currently queued in *commands and applies each
// to *buffer, stopping *loop_ptr on a quit command. Suspends between
// batches on new_commands->wait() - woken only when the poll-only
// periodic timer (main() below) has actually observed
// command_count move, never on a fixed schedule of its own. Named
// function with pointer parameters, not a capturing lambda coroutine -
// same reasoning examples/spreadsheet/main.cpp's own run_server() gives
// for its own coroutine (a reference or capture copied into a
// coroutine frame can still dangle if the referent doesn't outlive the
// frame; a pointer parameter says nothing implicit about that either
// way, matching cppcoreguidelines-avoid-reference-coroutine-parameters/
// -avoid-capturing-lambda-coroutines).
auto drain_commands(est::spsc_ring<larson_scanner::command>* commands,
                    est::external_event<std::size_t>* new_commands,
                    larson_scanner::led_buffer* buffer,
                    est::loop* loop_ptr) -> est::future<void> {
  for (;;) {
    co_await new_commands->wait();
    new_commands->reset();
    while (const auto cmd = commands->try_pop()) {
      if (cmd->kind == larson_scanner::CommandKind::quit) {
        loop_ptr->stop();
        co_return;
      }
      larson_scanner::apply(*buffer, *cmd);
    }
  }
}

} // namespace

auto main() -> int {
  // A concrete platform::interface isn't installed automatically just
  // by `import est;` - this program's own decision to make, same as
  // every other examples/*/main.cpp.
  estext::hosted_stdcpp platform_instance;
  const auto platform_guard = est::platform::override_instance(platform_instance);

  // stdout is fully buffered by default whenever it isn't a tty -
  // without this, a redirected/piped run would never actually show
  // the animation until the process exits. Same reasoning
  // examples/spreadsheet/main.cpp gives for the identical call.
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  using namespace std::chrono_literals;

  // Everything that can actually throw (spsc_ring<T>'s/led_buffer's own
  // allocations, schedule_periodic()'s precondition checks, std::jthread
  // construction, loop.run() itself) lives inside this one try block -
  // nothing throwing is reachable from main() outside it.
  try {
    est::loop loop;
    const auto loop_guard = est::make_current_loop(loop);

    larson_scanner::led_buffer buffer(40, 20.0F, 0.01F);

    // The controller: a real std::jthread genuinely blocks on
    // std::getline() (fine here - it's not the one thread est::loop
    // runs on), pushes each parsed command into commands, and bumps
    // command_count once per push so new_commands (an
    // est::external_event<std::size_t> bridging that counter) can wake
    // drain_commands() above - est::spsc_ring<T>'s own
    // "composition, not reuse" story (docs/wiki/Loop-And-Timers.md),
    // for real. Capacity 16 - comfortably more than a human can type
    // ahead of a drain.
    est::spsc_ring<larson_scanner::command> commands(16);
    std::atomic<std::size_t> command_count{0};
    est::external_event<std::size_t> new_commands{command_count};

    std::jthread input_thread([&commands, &command_count] {
      std::size_t pushed = 0;
      std::string line;
      while (std::getline(std::cin, line)) {
        if (const auto cmd = larson_scanner::parse_command(line)) {
          while (!commands.try_push(larson_scanner::command{*cmd})) {
            std::this_thread::yield(); // ring momentarily full - back off and retry
          }
          command_count.store(++pushed, std::memory_order_release);
        }
        // an unparseable line is silently dropped - never crash on bad input
      }
      // EOF (or a read error) also shuts the app down cleanly, via the
      // same quit command a literal "quit" line produces.
      while (
          !commands.try_push(larson_scanner::command{.kind = larson_scanner::CommandKind::quit})) {
        std::this_thread::yield();
      }
      command_count.store(++pushed, std::memory_order_release);
    });

    // The physics tick's own scheduling stays a regular fixed-interval
    // timer - tick_interval is reused as both schedule_periodic()'s own
    // interval and the dt argument led_buffer::tick() receives each
    // time, rather than led_buffer::tick() assuming any particular
    // cadence on its own (see its own doc comment in
    // larson_scanner.cppm for why that split matters).
    constexpr auto tick_interval = 20ms;
    auto tick_handle = est::schedule_periodic(
        tick_interval, [&buffer, tick_interval] { buffer.tick(tick_interval); });
    auto render_handle = est::schedule_periodic(
        33ms, [&buffer] { std::println("{}", larson_scanner::render(buffer)); });
    // poll() only - an O(1) load+compare, never touches commands itself;
    // the actual drain happens in drain_commands() above, once woken.
    auto poll_handle = est::schedule_periodic(20ms, [&new_commands] { new_commands.poll(); });

    auto commands_task = drain_commands(&commands, &new_commands, &buffer, &loop);
    loop.run();
  } catch (...) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
