import est;
import spreadsheet;
import std;

#include <catch2/catch_test_macros.hpp>

using spreadsheet::Verb;

TEST_CASE("parse_line: SET a value", "[protocol]") {
  const auto p = spreadsheet::parse_line("1 SET a 3.5");
  REQUIRE(p.id == "1");
  REQUIRE(p.v == Verb::set_value);
  REQUIRE(p.cell == 'a');
  REQUIRE(p.value == 3.5);
}

TEST_CASE("parse_line: SET a SUM with terms", "[protocol]") {
  const auto p = spreadsheet::parse_line("2 SET c SUM a b");
  REQUIRE(p.id == "2");
  REQUIRE(p.v == Verb::set_sum);
  REQUIRE(p.cell == 'c');
  REQUIRE(p.terms == std::vector<char>{'a', 'b'});
}

TEST_CASE("parse_line: SET a SUM with no terms is valid", "[protocol]") {
  const auto p = spreadsheet::parse_line("3 SET a SUM");
  REQUIRE(p.v == Verb::set_sum);
  REQUIRE(p.terms.empty());
}

TEST_CASE("parse_line: UNSET", "[protocol]") {
  const auto p = spreadsheet::parse_line("4 UNSET a");
  REQUIRE(p.v == Verb::unset);
  REQUIRE(p.cell == 'a');
}

TEST_CASE("parse_line: GET", "[protocol]") {
  const auto p = spreadsheet::parse_line("5 GET a");
  REQUIRE(p.v == Verb::get);
  REQUIRE(p.cell == 'a');
}

TEST_CASE("parse_line: GET BLOCKING", "[protocol]") {
  const auto p = spreadsheet::parse_line("6 GET BLOCKING a");
  REQUIRE(p.v == Verb::get_blocking);
  REQUIRE(p.cell == 'a');
}

TEST_CASE("parse_line: EXIT", "[protocol]") {
  const auto p = spreadsheet::parse_line("7 EXIT");
  REQUIRE(p.v == Verb::exit_cmd);
}

TEST_CASE("parse_line: a blank line has no id and is invalid", "[protocol]") {
  const auto p = spreadsheet::parse_line("   ");
  REQUIRE(p.id.empty());
  REQUIRE(p.v == Verb::invalid);
}

TEST_CASE("parse_line: unknown verb is invalid but keeps the id", "[protocol]") {
  const auto p = spreadsheet::parse_line("8 FROB a");
  REQUIRE(p.id == "8");
  REQUIRE(p.v == Verb::invalid);
}

TEST_CASE("parse_line: cell id outside a-z is invalid", "[protocol]") {
  REQUIRE(spreadsheet::parse_line("9 SET A 1").v == Verb::invalid);
  REQUIRE(spreadsheet::parse_line("9 SET 1 1").v == Verb::invalid);
  REQUIRE(spreadsheet::parse_line("9 GET ab").v == Verb::invalid);
}

TEST_CASE("parse_line: non-numeric SET value is invalid", "[protocol]") {
  REQUIRE(spreadsheet::parse_line("10 SET a xyz").v == Verb::invalid);
}

TEST_CASE("parse_line: wrong argument count is invalid", "[protocol]") {
  REQUIRE(spreadsheet::parse_line("11 SET a").v == Verb::invalid);
  REQUIRE(spreadsheet::parse_line("11 SET a 1 2").v == Verb::invalid);
  REQUIRE(spreadsheet::parse_line("11 UNSET").v == Verb::invalid);
  REQUIRE(spreadsheet::parse_line("11 GET").v == Verb::invalid);
  REQUIRE(spreadsheet::parse_line("11 EXIT extra").v == Verb::invalid);
}

TEST_CASE("execute: SET responds OK", "[protocol]") {
  est::loop loop;
  const auto guard = est::make_current_loop(loop);
  spreadsheet::sheet sheet;

  const auto response = spreadsheet::execute(&sheet, spreadsheet::parse_line("1 SET a 3.5")).get();
  REQUIRE(response == "1 OK");
  REQUIRE(sheet.resolve('a') == 3.5);
}

TEST_CASE("execute: SET SUM cycle responds ERROR DEPS", "[protocol]") {
  est::loop loop;
  const auto guard = est::make_current_loop(loop);
  spreadsheet::sheet sheet;

  REQUIRE(spreadsheet::execute(&sheet, spreadsheet::parse_line("1 SET a SUM b")).get() == "1 OK");
  const auto response =
      spreadsheet::execute(&sheet, spreadsheet::parse_line("2 SET b SUM a")).get();
  REQUIRE(response == "2 ERROR DEPS");
}

TEST_CASE("execute: UNSET responds OK", "[protocol]") {
  est::loop loop;
  const auto guard = est::make_current_loop(loop);
  spreadsheet::sheet sheet;
  sheet.set_value('a', 1.0);

  const auto response = spreadsheet::execute(&sheet, spreadsheet::parse_line("1 UNSET a")).get();
  REQUIRE(response == "1 OK");
  REQUIRE_THROWS_AS(sheet.resolve('a'), spreadsheet::not_defined);
}

TEST_CASE("execute: GET on an undefined cell responds ERROR NOT DEFINED", "[protocol]") {
  est::loop loop;
  const auto guard = est::make_current_loop(loop);
  spreadsheet::sheet sheet;

  const auto response = spreadsheet::execute(&sheet, spreadsheet::parse_line("1 GET a")).get();
  REQUIRE(response == "1 ERROR NOT DEFINED a");
}

TEST_CASE("execute: GET on a defined cell responds OK with the value", "[protocol]") {
  est::loop loop;
  const auto guard = est::make_current_loop(loop);
  spreadsheet::sheet sheet;
  sheet.set_value('a', 3.5);

  const auto response = spreadsheet::execute(&sheet, spreadsheet::parse_line("1 GET a")).get();
  REQUIRE(response == "1 OK a 3.5");
}

TEST_CASE("execute: value formatting is shortest round-trip", "[protocol]") {
  est::loop loop;
  const auto guard = est::make_current_loop(loop);
  spreadsheet::sheet sheet;
  sheet.set_value('a', 3.0);

  const auto response = spreadsheet::execute(&sheet, spreadsheet::parse_line("1 GET a")).get();
  REQUIRE(response == "1 OK a 3");
}

TEST_CASE("execute: GET BLOCKING does not block later commands from being dispatched",
          "[protocol]") {
  est::loop loop;
  const auto guard = est::make_current_loop(loop);
  spreadsheet::sheet sheet;

  auto blocked = spreadsheet::execute(&sheet, spreadsheet::parse_line("1 GET BLOCKING a"));
  REQUIRE_FALSE(blocked.ready());

  // A later command is still handled immediately, proving GET BLOCKING
  // never stalled the (conceptual) read loop that dispatched it.
  const auto immediate = spreadsheet::execute(&sheet, spreadsheet::parse_line("2 SET b 1")).get();
  REQUIRE(immediate == "2 OK");

  (void)spreadsheet::execute(&sheet, spreadsheet::parse_line("3 SET a 6.0")).get();
  loop.run_until_idle();

  REQUIRE(blocked.ready());
  REQUIRE(blocked.get() == "1 OK a 6");
}
