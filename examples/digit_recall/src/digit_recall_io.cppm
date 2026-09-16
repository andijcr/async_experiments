// POSIX read()/errno/EAGAIN/STDIN_FILENO - like EXIT_SUCCESS elsewhere in
// this codebase, these are macros/plain declarations `import std;`
// doesn't carry. Included here, in the global module fragment (the only
// place a non-modular header belongs), rather than after `export module`.
module;
#include <cerrno>
#include <unistd.h>

export module digit_recall_io;

import est;
import std;

// read_line_async(): the exact shape examples/spreadsheet/src/
// spreadsheet_io.cppm's own getline_async() already established and
// documented (raw, non-blocking ::read(), 100ms poll interval -
// std::cin's own public API can't tell "no data yet" from real EOF once
// STDIN_FILENO is O_NONBLOCK, confirmed against the real pinned
// toolchain there) - duplicated here rather than shared, matching this
// codebase's existing "each example is self-contained" convention
// (spreadsheet_io.cppm's own module isn't reused by any other example
// either). Kept in its own module, separate from digit_recall.cppm, for
// the identical reason spreadsheet_io.cppm is: it talks to a real fd, so
// unlike everything in digit_recall.cppm it isn't unit-tested directly -
// only exercised end to end by actually running the program.
export namespace digit_recall {

struct end_of_input final : std::exception {
  [[nodiscard]] auto what() const noexcept -> const char* override { return "end of input"; }
};

[[nodiscard]] auto read_line_async() -> est::future<std::string> {
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

} // namespace digit_recall
