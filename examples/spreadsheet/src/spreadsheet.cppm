export module spreadsheet;

import est;
import std;

// The whole spreadsheet protocol (docs/PLAN.md's "spreadsheet server"
// example) lives here as plain, synchronous-looking logic (sheet's own
// methods, parse_line()) plus one thin coroutine (execute()) that bridges
// it into est::future<std::string> for the async command/response flow
// main.cpp drives. Deliberately its own module, separate from
// spreadsheet_io (getline_async()): this half is pure enough to unit-test
// directly (examples/spreadsheet/tests/), the I/O half isn't (it talks to
// a real fd).
export namespace spreadsheet {

[[nodiscard]] constexpr auto is_cell(char c) noexcept -> bool {
  return c >= 'a' && c <= 'z';
}

// Thrown by sheet::resolve() (and caught internally by
// resolve_blocking()) naming the specific cell - possibly several links
// down a SUM chain - that has no value yet. Also what execute() catches
// to build "ERROR NOT DEFINED <cell>" for a plain (non-blocking) GET.
struct not_defined final : std::exception {
  explicit not_defined(char cell_id) noexcept : cell(cell_id) {}
  [[nodiscard]] auto what() const noexcept -> const char* override { return "cell not defined"; }
  char cell;
};

// The command verbs the protocol defines, plus `invalid` for anything
// parse_line() couldn't make sense of (bad keyword, wrong arg count, a
// cell id outside a-z, a non-numeric SET value) - never thrown, just
// reported so the caller can respond with a generic ERROR and keep
// reading, per the "never crash" requirement. CamelCase (.clang-tidy's
// EnumCase, uint8_t base per performance-enum-size) - this project's
// first enum, everything else here otherwise matches the codebase's own
// lower_snake_case.
enum class Verb : std::uint8_t { set_value, set_sum, unset, get, get_blocking, exit_cmd, invalid };

// One parsed command line. `id` is the caller-supplied unique id, empty
// only for a blank/whitespace-only line (nothing to even echo back).
// `terms` is only meaningful for set_sum; `cell`/`value` only for the
// verbs that use them - left at their default otherwise.
struct parsed_line {
  std::string id;
  Verb v = Verb::invalid;
  char cell = 0;
  double value = 0.0;
  std::vector<char> terms;
};

} // namespace spreadsheet

// Per-verb parsing helpers, kept out of the exported namespace: splitting
// parse_line() this way (rather than one large function walking every
// verb inline) is what keeps its own cognitive complexity - and each
// helper's - well under readability-function-cognitive-complexity's
// threshold. Every container access below goes through .at() rather than
// operator[] (cppcoreguidelines-pro-bounds-avoid-unchecked-container-
// access) - never actually throws given each call site's own size
// checks, but costs nothing here and satisfies the check without
// weakening those checks into something less precise.
namespace spreadsheet::detail {

[[nodiscard]] auto tokenize(std::string_view line) -> std::vector<std::string_view> {
  std::vector<std::string_view> tokens;
  std::size_t pos = 0;
  while (pos < line.size()) {
    while (pos < line.size() && (line.at(pos) == ' ' || line.at(pos) == '\t')) {
      ++pos;
    }
    const std::size_t start = pos;
    while (pos < line.size() && line.at(pos) != ' ' && line.at(pos) != '\t') {
      ++pos;
    }
    if (pos > start) {
      tokens.push_back(line.substr(start, pos - start));
    }
  }
  return tokens;
}

[[nodiscard]] auto as_cell(std::string_view tok, char& out) -> bool {
  if (tok.size() != 1 || !is_cell(tok.at(0))) {
    return false;
  }
  out = tok.at(0);
  return true;
}

void parse_exit(const std::vector<std::string_view>& tokens, parsed_line& result) {
  if (tokens.size() == 2) {
    result.v = Verb::exit_cmd;
  }
}

void parse_set(const std::vector<std::string_view>& tokens, parsed_line& result) {
  if (tokens.size() < 4 || !as_cell(tokens.at(2), result.cell)) {
    return;
  }
  if (tokens.at(3) == "SUM") {
    std::vector<char> terms;
    terms.reserve(tokens.size() - 4);
    for (std::size_t i = 4; i < tokens.size(); ++i) {
      char term = 0;
      if (!as_cell(tokens.at(i), term)) {
        return; // stays Verb::invalid
      }
      terms.push_back(term);
    }
    result.v = Verb::set_sum;
    result.terms = std::move(terms);
    return;
  }
  if (tokens.size() != 4) {
    return;
  }
  // std::from_chars<double> would be the obvious choice here, but the
  // pinned devenv's libc++.so doesn't export
  // __from_chars_floating_point (present only in the static libc++.a,
  // confirmed against the real toolchain) - std::stod(), backed by the
  // C library's strtod() instead, has no such gap. It throws on a
  // genuinely non-numeric token, which fits this function's own
  // never-throw/report-invalid-instead contract exactly.
  const std::string value_tok(tokens.at(3));
  std::size_t chars_consumed = 0;
  double value = 0.0;
  try {
    value = std::stod(value_tok, &chars_consumed);
  } catch (const std::exception&) {
    return;
  }
  if (chars_consumed != value_tok.size()) {
    return;
  }
  result.v = Verb::set_value;
  result.value = value;
}

void parse_unset(const std::vector<std::string_view>& tokens, parsed_line& result) {
  if (tokens.size() == 3 && as_cell(tokens.at(2), result.cell)) {
    result.v = Verb::unset;
  }
}

void parse_get(const std::vector<std::string_view>& tokens, parsed_line& result) {
  if (tokens.size() == 3 && as_cell(tokens.at(2), result.cell)) {
    result.v = Verb::get;
  } else if (tokens.size() == 4 && tokens.at(2) == "BLOCKING" &&
             as_cell(tokens.at(3), result.cell)) {
    result.v = Verb::get_blocking;
  }
}

} // namespace spreadsheet::detail

export namespace spreadsheet {

// Splits `line` on whitespace and interprets the tokens against the
// protocol grammar. Never throws - a line that doesn't fit comes back as
// `Verb::invalid` (with `id` set to the first token, if there was one, so
// the caller can still echo it in an ERROR response).
[[nodiscard]] auto parse_line(std::string_view line) -> parsed_line {
  const auto tokens = detail::tokenize(line);

  parsed_line result;
  if (tokens.empty()) {
    return result;
  }
  result.id = std::string(tokens.at(0));
  if (tokens.size() < 2) {
    return result;
  }

  const std::string_view verb_tok = tokens.at(1);
  if (verb_tok == "EXIT") {
    detail::parse_exit(tokens, result);
  } else if (verb_tok == "SET") {
    detail::parse_set(tokens, result);
  } else if (verb_tok == "UNSET") {
    detail::parse_unset(tokens, result);
  } else if (verb_tok == "GET") {
    detail::parse_get(tokens, result);
  }
  return result;
}

// The store: each cell is either a direct value or a SUM formula (a list
// of other cell ids) - never both, since a fresh SET on an already-set
// cell replaces its definition outright. Backed by a std::unordered_map
// keyed on the cell id, as asked for.
class sheet {
public:
  void set_value(char id, double value) {
    est::check(is_cell(id), "spreadsheet::sheet: cell id must be in 'a'..'z'");
    cells_.insert_or_assign(id, value);
    notify_presence(id);
  }

  // Returns false (and leaves the sheet unchanged) if defining `id` as
  // SUM(terms) would create a dependency cycle - execute() turns that
  // into "ERROR DEPS" rather than committing it.
  [[nodiscard]] auto set_sum(char id, std::vector<char> terms) -> bool {
    est::check(is_cell(id), "spreadsheet::sheet: cell id must be in 'a'..'z'");
    if (creates_cycle(id, terms)) {
      return false;
    }
    cells_.insert_or_assign(id, std::move(terms));
    notify_presence(id);
    return true;
  }

  void unset(char id) {
    est::check(is_cell(id), "spreadsheet::sheet: cell id must be in 'a'..'z'");
    cells_.erase(id);
  }

  // Synchronous resolution: recurses through SUM formulas (memoized per
  // call, not across calls - SET/UNSET can change the graph between
  // calls) and throws not_defined{missing_cell} the first time it hits a
  // cell that isn't in the store at all. Cycle-free by construction
  // (set_sum() above never lets a cycle in), so this always terminates.
  [[nodiscard]] auto resolve(char id) const -> double {
    std::unordered_map<char, double> memo;
    return resolve_impl(id, memo);
  }

  // Coroutine wrapper around resolve(): retries the whole synchronous
  // resolution from scratch each time it catches a not_defined, awaiting
  // presence of exactly the cell that was reported missing, rather than
  // re-implementing the SUM-walk as a second, separately-maintained
  // async algorithm.
  [[nodiscard]] auto resolve_blocking(char id) -> est::future<double> {
    for (;;) {
      // co_await isn't allowed inside a catch handler (a coroutine can't
      // suspend mid-unwind), so the missing cell is captured here and
      // awaited only after the try/catch has fully exited.
      char missing_cell = 0;
      try {
        co_return resolve(id);
      } catch (const not_defined& missing) {
        missing_cell = missing.cell;
      }
      co_await wait_for_presence(missing_cell);
    }
  }

private:
  // One shared promise/future pair per cell, not one per waiter (PR #53
  // review): future_state<T>'s own waiters_ list already supports any
  // number of independent registrations against a single future - every
  // .then()/co_await on the same future<T> lvalue just appends another
  // continuation, all fired together once it completes (future.cppm,
  // future_state<T>::set_continuation()/complete()). Safe specifically
  // because this is future<void> - nothing to consume, so N waiters each
  // calling get() (via co_await) off the same future_state is fine; it
  // would not be for a value-carrying future<T>, whose move-only, single-
  // consumer contract this deliberately doesn't disturb.
  //
  // Heap-allocated behind a shared_ptr, not stored by value in the map:
  // a co_await'ing coroutine's own future_awaiter holds a *reference* to
  // this future<void> object itself (not just its future_state) that has
  // to stay valid until that specific coroutine actually resumes - which
  // happens later, on a subsequent loop drain, not by the time
  // notify_presence() (below) returns. An earlier version stored
  // waiter_slot by value in the map and moved it out in notify_presence()
  // to fire it, which let the *map entry* - and so the future<void> every
  // still-suspended waiter's awaiter pointed at - be erased before those
  // waiters ever resumed: a real, reproducible (SEGFAULT under the unit
  // tests) use-after-free. Each wait_for_presence() call now keeps its
  // own shared_ptr copy alive in its own coroutine frame across the
  // suspension, so the slot outlives notify_presence() erasing the map's
  // own reference to it, for as long as the last waiter needs it to.
  struct waiter_slot {
    est::promise<void> prom;
    est::future<void> fut;
  };

  // A waiter for `id` is only ever registered (see resolve_blocking()
  // above) while cells_ does not yet contain `id` - so the first SET that
  // makes it present is always the transition being waited for, and
  // notify_presence() firing every registered waiter unconditionally is
  // never a spurious wakeup.
  [[nodiscard]] auto wait_for_presence(char id) -> est::future<void> {
    if (cells_.contains(id)) {
      co_return;
    }
    auto it = presence_waiters_.find(id);
    if (it == presence_waiters_.end()) {
      auto [prom, fut] = est::make_promise_future<void>();
      it = presence_waiters_
               .emplace(id, std::make_shared<waiter_slot>(std::move(prom), std::move(fut)))
               .first;
    }
    const auto slot = it->second;
    co_await slot->fut;
  }

  void notify_presence(char id) {
    const auto it = presence_waiters_.find(id);
    if (it == presence_waiters_.end()) {
      return;
    }
    const auto slot = std::move(it->second);
    presence_waiters_.erase(it);
    slot->prom.set_value();
  }

  [[nodiscard]] auto resolve_impl(char id, std::unordered_map<char, double>& memo) const -> double {
    if (const auto memo_it = memo.find(id); memo_it != memo.end()) {
      return memo_it->second;
    }
    const auto cell_it = cells_.find(id);
    if (cell_it == cells_.end()) {
      throw not_defined(id);
    }
    double result = 0.0;
    if (const auto* value = std::get_if<double>(&cell_it->second)) {
      result = *value;
    } else {
      for (const char term : std::get<std::vector<char>>(cell_it->second)) {
        result += resolve_impl(term, memo);
      }
    }
    memo.emplace(id, result);
    return result;
  }

  // True if any of `terms`, or anything reachable from them by following
  // *existing* SUM edges, is `target` itself - i.e. whether an edge
  // target -> terms would close a cycle. Deliberately never follows
  // target's own current outgoing edges (only what terms already points
  // to), so redefining an already-set cell only ever checks the new
  // formula, not the one being replaced.
  [[nodiscard]] auto creates_cycle(char target, const std::vector<char>& terms) const -> bool {
    std::vector<char> stack(terms.begin(), terms.end());
    std::array<bool, 26> visited{};
    while (!stack.empty()) {
      const char current = stack.back();
      stack.pop_back();
      if (current == target) {
        return true;
      }
      auto& seen = visited.at(static_cast<std::size_t>(current - 'a'));
      if (seen) {
        continue;
      }
      seen = true;
      const auto it = cells_.find(current);
      if (it == cells_.end()) {
        continue;
      }
      if (const auto* sum_terms = std::get_if<std::vector<char>>(&it->second)) {
        stack.insert(stack.end(), sum_terms->begin(), sum_terms->end());
      }
    }
    return false;
  }

  std::unordered_map<char, std::variant<double, std::vector<char>>> cells_;
  std::unordered_map<char, std::shared_ptr<waiter_slot>> presence_waiters_;
};

// Bridges one parsed_line into the sheet, returning the full response
// line (including the echoed id) to print. The only verbs handled here
// are the ones that touch the sheet - exit_cmd/invalid/a blank line are
// main.cpp's own concern (control flow over the read loop itself, not a
// sheet operation).
//
// Both parameters are taken by value/pointer rather than by reference:
// coroutine parameters are copied into the frame at coroutine-start
// (only their initializer expression's side effects, if any, are
// guaranteed to have already happened) but a *reference* parameter
// itself dangles the instant the referent's own lifetime ends - and
// main.cpp's read loop constructs a fresh `parsed_line` every iteration,
// which does not outlive this coroutine's own suspension across GET
// BLOCKING. `sheet` is safe to keep as a genuine reference in a
// non-coroutine function, but execute() itself co_awaits, so it takes a
// pointer instead (cppcoreguidelines-avoid-reference-coroutine-
// parameters) - main.cpp's `sheet_instance` still outlives every command
// dispatched against it either way.
[[nodiscard]] auto execute(sheet* s, parsed_line parsed) -> est::future<std::string> {
  switch (parsed.v) {
    case Verb::set_value:
      s->set_value(parsed.cell, parsed.value);
      co_return std::format("{} OK", parsed.id);

    case Verb::set_sum:
      if (s->set_sum(parsed.cell, parsed.terms)) {
        co_return std::format("{} OK", parsed.id);
      }
      co_return std::format("{} ERROR DEPS", parsed.id);

    case Verb::unset:
      s->unset(parsed.cell);
      co_return std::format("{} OK", parsed.id);

    case Verb::get:
      try {
        co_return std::format("{} OK {} {}", parsed.id, parsed.cell, s->resolve(parsed.cell));
      } catch (const not_defined& missing) {
        co_return std::format("{} ERROR NOT DEFINED {}", parsed.id, missing.cell);
      }

    case Verb::get_blocking: {
      const double value = co_await s->resolve_blocking(parsed.cell);
      co_return std::format("{} OK {} {}", parsed.id, parsed.cell, value);
    }

    case Verb::exit_cmd:
    case Verb::invalid:
      break;
  }
  // Unreachable via main.cpp (it never calls execute() for these two
  // verbs - see this function's own doc comment) but still a valid,
  // total answer for any other caller (e.g. a unit test exercising
  // execute() directly against Verb::invalid).
  co_return std::format("{} ERROR", parsed.id);
}

} // namespace spreadsheet
