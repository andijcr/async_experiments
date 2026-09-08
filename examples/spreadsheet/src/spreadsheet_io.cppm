// POSIX read()/errno/EAGAIN/STDIN_FILENO - like EXIT_SUCCESS elsewhere in
// this codebase, these are macros/plain declarations `import std;`
// doesn't carry. Included here, in the global module fragment (the only
// place a non-modular header belongs - attached to no module rather than
// silently attached to this one), rather than after `export module`.
module;
#include <cerrno>
#include <unistd.h>

export module spreadsheet_io;

import est;
import std;

// getline_async() is kept in its own module, separate from spreadsheet
// (spreadsheet.cppm): it talks to a real fd, so unlike everything in
// that module it isn't unit-tested directly - only exercised end to end
// by actually running the program.
export namespace spreadsheet {

// Thrown by getline_async() once stdin has reached real, permanent EOF -
// the sentinel main.cpp's read loop catches to know no more commands
// will ever arrive and it's time to shut down.
struct end_of_input final : std::exception {
  [[nodiscard]] auto what() const noexcept -> const char* override { return "end of input"; }
};

// Reads one newline-terminated line from stdin, one byte at a time,
// suspending for 100ms between polls when nothing is available yet
// rather than ever blocking the one thread est::loop runs on - exactly
// the shape asked for, except the actual byte read is a raw ::read()
// rather than going through std::cin.
//
// That deviation is load-bearing, not stylistic: with STDIN_FILENO set
// O_NONBLOCK (main()'s job, once, before this is ever called), std::cin's
// own public API - in_avail(), get(), rdbuf()->sbumpc() - was tried and,
// on this toolchain, cannot tell "no data yet" apart from "real EOF":
// both report back to the caller identically (in_avail() always reports
// 0 either way; get() sets eofbit/failbit either way). A raw ::read()'s
// return value alone distinguishes the three cases unambiguously (>0
// bytes read, 0 = real EOF, -1 with errno EAGAIN/EWOULDBLOCK = not yet),
// so it's used here instead - confirmed against the real pinned
// toolchain before settling on this.
[[nodiscard]] auto getline_async() -> est::future<std::string> {
  std::string line;
  using namespace std::chrono_literals;
  for (;;) {
    char ch = 0;
    const ssize_t n = ::read(STDIN_FILENO, &ch, 1);
    if (n == 1) {
      if (ch == '\n') {
        co_return line;
      }
      line.push_back(ch);
      continue;
    }
    if (n == 0) {
      throw end_of_input{};
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      throw end_of_input{};
    }
    co_await est::sleep_for(100ms);
  }
}

} // namespace spreadsheet
