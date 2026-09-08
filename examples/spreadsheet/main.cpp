import est;
import estext;
import spreadsheet;
import spreadsheet_io;
import std;

// EXIT_SUCCESS/EXIT_FAILURE and the POSIX fcntl()/O_NONBLOCK/STDIN_FILENO
// setup below are macros/plain declarations `import std;` doesn't carry
// (same reasoning as examples/hello_world, examples/sleep_sort), so this
// stays a classic #include.
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

namespace {

// The read loop: repeatedly awaits one line, dispatches it, and loops
// back immediately rather than awaiting the dispatch - that's what makes
// GET BLOCKING not block later commands from being read and processed
// while it's still waiting on a dependency. Each dispatched command's
// response is printed from a then() continuation once it's ready,
// whichever order they actually resolve in.
//
// Takes sheet_instance by pointer, not reference: a coroutine's reference
// parameters are flagged by cppcoreguidelines-avoid-reference-coroutine-
// parameters for good reason (they dangle the instant the referent's
// lifetime ends, which a reference parameter's own copy into the
// coroutine frame does nothing to prevent) - main()'s sheet_instance
// genuinely does outlive every coroutine dispatched against it, but a
// pointer says so without relying on the check's blind spot for
// reference parameters specifically.
auto run_server(spreadsheet::sheet* sheet_instance) -> est::future<void> {
  for (;;) {
    std::string line;
    try {
      line = co_await spreadsheet::getline_async();
    } catch (const spreadsheet::end_of_input&) {
      break;
    }

    auto parsed = spreadsheet::parse_line(line);
    if (parsed.id.empty()) {
      continue; // blank line: nothing to even echo back
    }
    if (parsed.v == spreadsheet::Verb::exit_cmd) {
      std::println("{} OK", parsed.id);
      break;
    }
    if (parsed.v == spreadsheet::Verb::invalid) {
      std::println("{} ERROR", parsed.id);
      continue;
    }

    // Deliberately discarded: the dispatched command's own future_state
    // (and, for GET BLOCKING, its pending resolve_blocking() coroutine)
    // stays alive on its own via the then() continuation queued on it -
    // see docs/wiki/Allocation-Patterns.md - not via this handle.
    spreadsheet::execute(sheet_instance, std::move(parsed)).then([](const std::string& response) {
      std::println("{}", response);
    });
  }
}

} // namespace

auto main() -> int {
  // A concrete platform::interface isn't installed automatically just by
  // `import est;` - this program's own decision to make (examples/
  // hello_world/main.cpp, examples/sleep_sort/main.cpp do the same).
  estext::hosted_stdcpp platform_instance;
  const auto platform_guard = est::platform::override_instance(platform_instance);

  // stdout is fully buffered by default whenever it isn't a tty - the
  // normal case for this program, piped to whatever sent the commands -
  // so without this, every response std::println() below writes sits in
  // libc's buffer, invisible to the caller, until enough of them
  // accumulate to fill it or the process exits. That defeats the whole
  // "answers can come interleaved" point of the protocol: a caller
  // expecting a prompt response to command N would see nothing until
  // command N+50 or EXIT. Line-buffering forces a flush after every '\n'
  // std::println() writes, matching a tty's own default behavior instead
  // of a pipe's.
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  // getline_async() polls stdin with a raw, non-blocking ::read() rather
  // than ever blocking the one thread est::loop runs on - this is the
  // one-time setup that makes that safe (spreadsheet_io.cppm's own doc
  // comment has the full story on why std::cin couldn't be used as-is).
  // fcntl() is a C-style vararg function - no non-vararg alternative
  // exists for this POSIX API, so cppcoreguidelines-pro-type-vararg is
  // unavoidable at this OS boundary rather than a real type-safety risk
  // here (both calls' third argument is a plain int).
  // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg)
  const int flags = ::fcntl(STDIN_FILENO, F_GETFL, 0);
  ::fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
  // NOLINTEND(cppcoreguidelines-pro-type-vararg)

  est::loop loop;
  const auto loop_guard = est::make_current_loop(loop);
  spreadsheet::sheet sheet_instance;

  try {
    auto server = run_server(&sheet_instance);
    loop.run();
  } catch (...) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
