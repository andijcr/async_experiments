import est;
import spreadsheet;
import std;

#include <catch2/catch_test_macros.hpp>

TEST_CASE("set_value then resolve returns the value", "[sheet]") {
  spreadsheet::sheet sheet;
  sheet.set_value('a', 3.5);
  REQUIRE(sheet.resolve('a') == 3.5);
}

TEST_CASE("resolve throws not_defined for a cell never set", "[sheet]") {
  spreadsheet::sheet sheet;
  try {
    (void)sheet.resolve('a');
    FAIL("expected not_defined");
  } catch (const spreadsheet::not_defined& e) {
    REQUIRE(e.cell == 'a');
  }
}

TEST_CASE("unset removes a cell", "[sheet]") {
  spreadsheet::sheet sheet;
  sheet.set_value('a', 1.0);
  sheet.unset('a');
  REQUIRE_THROWS_AS(sheet.resolve('a'), spreadsheet::not_defined);
}

TEST_CASE("set_sum resolves as the sum of its terms", "[sheet]") {
  spreadsheet::sheet sheet;
  sheet.set_value('a', 1.0);
  sheet.set_value('b', 2.5);
  REQUIRE(sheet.set_sum('c', {'a', 'b'}));
  REQUIRE(sheet.resolve('c') == 3.5);
}

TEST_CASE("SET SUM with no terms sums to 0.0", "[sheet]") {
  spreadsheet::sheet sheet;
  REQUIRE(sheet.set_sum('a', {}));
  REQUIRE(sheet.resolve('a') == 0.0);
}

TEST_CASE("set_sum resolves through a chain of formulas", "[sheet]") {
  spreadsheet::sheet sheet;
  sheet.set_value('a', 1.0);
  REQUIRE(sheet.set_sum('b', {'a'}));
  REQUIRE(sheet.set_sum('c', {'b', 'a'}));
  REQUIRE(sheet.resolve('c') == 2.0);
}

TEST_CASE("resolve through a formula throws not_defined naming the missing leaf", "[sheet]") {
  spreadsheet::sheet sheet;
  REQUIRE(sheet.set_sum('a', {'b'}));
  REQUIRE(sheet.set_sum('b', {'c'}));
  try {
    (void)sheet.resolve('a');
    FAIL("expected not_defined");
  } catch (const spreadsheet::not_defined& e) {
    REQUIRE(e.cell == 'c');
  }
}

TEST_CASE("set_sum rejects a direct self-reference", "[sheet]") {
  spreadsheet::sheet sheet;
  REQUIRE_FALSE(sheet.set_sum('a', {'a'}));
  REQUIRE_THROWS_AS(sheet.resolve('a'), spreadsheet::not_defined);
}

TEST_CASE("set_sum rejects an indirect cycle", "[sheet]") {
  spreadsheet::sheet sheet;
  REQUIRE(sheet.set_sum('a', {'b'}));
  REQUIRE_FALSE(sheet.set_sum('b', {'a'}));
}

TEST_CASE("redefining a cell only checks its new formula, not the one it replaces", "[sheet]") {
  spreadsheet::sheet sheet;
  // a depends on b; redefining a to no longer depend on b must not be
  // blocked by a's own stale (about-to-be-replaced) edge to b.
  REQUIRE(sheet.set_sum('a', {'b'}));
  sheet.set_value('a', 5.0);
  // Now b can freely depend on a - a's old formula is gone, so this is
  // not a cycle.
  REQUIRE(sheet.set_sum('b', {'a'}));
  REQUIRE(sheet.resolve('b') == 5.0);
}

TEST_CASE("resolve_blocking resolves immediately when already defined", "[sheet]") {
  est::loop loop;
  const auto guard = est::make_current_loop(loop);
  spreadsheet::sheet sheet;
  sheet.set_value('a', 7.0);

  auto fut = sheet.resolve_blocking('a');
  REQUIRE(fut.ready());
  REQUIRE(fut.get() == 7.0);
}

TEST_CASE("resolve_blocking waits for a direct dependency to be set", "[sheet]") {
  est::loop loop;
  const auto guard = est::make_current_loop(loop);
  spreadsheet::sheet sheet;

  auto fut = sheet.resolve_blocking('a');
  REQUIRE_FALSE(fut.ready());

  sheet.set_value('a', 4.5);
  loop.run_until_idle();

  REQUIRE(fut.ready());
  REQUIRE(fut.get() == 4.5);
}

TEST_CASE("resolve_blocking waits for an indirect dependency several links down", "[sheet]") {
  est::loop loop;
  const auto guard = est::make_current_loop(loop);
  spreadsheet::sheet sheet;

  REQUIRE(sheet.set_sum('b', {'c'}));
  REQUIRE(sheet.set_sum('a', {'b'}));

  auto fut = sheet.resolve_blocking('a');
  REQUIRE_FALSE(fut.ready());

  sheet.set_value('c', 2.0);
  loop.run_until_idle();

  REQUIRE(fut.ready());
  REQUIRE(fut.get() == 2.0);
}

TEST_CASE("multiple blocked reads on the same cell all resolve once it is set", "[sheet]") {
  est::loop loop;
  const auto guard = est::make_current_loop(loop);
  spreadsheet::sheet sheet;

  auto first = sheet.resolve_blocking('a');
  auto second = sheet.resolve_blocking('a');
  REQUIRE_FALSE(first.ready());
  REQUIRE_FALSE(second.ready());

  sheet.set_value('a', 9.0);
  loop.run_until_idle();

  REQUIRE(first.ready());
  REQUIRE(second.ready());
  REQUIRE(first.get() == 9.0);
  REQUIRE(second.get() == 9.0);
}
