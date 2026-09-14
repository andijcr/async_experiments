import larson_scanner;
import std;

#include <catch2/catch_test_macros.hpp>

TEST_CASE("scanner_channel: starts fully unlit", "[larson_scanner]") {
  larson_scanner::scanner_channel ch(3, 1.0F, 0.0F);
  for (const float v : ch.intensities()) {
    REQUIRE(v == 0.0F);
  }
}

TEST_CASE("scanner_channel: bounces between both ends of the strip", "[larson_scanner]") {
  // decay 0 isolates exactly one lit pixel per tick (the current
  // position), making the bounce sequence fully deterministic: with
  // width 3 and speed 1, the lit index should trace 0, 1, 2, 1, 0.
  larson_scanner::scanner_channel ch(3, 1.0F, 0.0F);
  const std::array<std::size_t, 5> expected_lit_index{0, 1, 2, 1, 0};

  for (const std::size_t lit : expected_lit_index) {
    ch.tick();
    std::vector<float> expected(3, 0.0F);
    expected.at(lit) = 1.0F;
    REQUIRE(std::ranges::equal(ch.intensities(), expected));
  }
}

TEST_CASE("scanner_channel: decay reduces intensity at pixels the scanner has moved away from",
          "[larson_scanner]") {
  larson_scanner::scanner_channel ch(3, 1.0F, 0.5F);
  ch.tick(); // pins index 0 to 1.0, advances to position 1
  ch.tick(); // decays index 0 to 0.5, pins index 1 to 1.0

  REQUIRE(std::ranges::equal(ch.intensities(), std::array{0.5F, 1.0F, 0.0F}));
}

TEST_CASE("scanner_channel: a stationary scanner (speed 0) stays pinned at its own position",
          "[larson_scanner]") {
  larson_scanner::scanner_channel ch(3, 0.0F, 0.5F);
  ch.tick();
  ch.tick();
  ch.tick();

  // re-pinned to full brightness every tick - decay never shows at index 0
  REQUIRE(std::ranges::equal(ch.intensities(), std::array{1.0F, 0.0F, 0.0F}));
}

TEST_CASE("led_buffer: ticks all three channels independently", "[larson_scanner]") {
  larson_scanner::led_buffer buffer(5, 1.0F, 0.0F);
  buffer.red.speed = 2.0F; // diverge red from green/blue *before* the first tick

  buffer.tick(); // all three pin index 0 (starting position, unaffected by speed)
  buffer.tick(); // red has since advanced by 2, green/blue by 1 - now they diverge

  REQUIRE(std::ranges::equal(buffer.red.intensities(), std::array{0.0F, 0.0F, 1.0F, 0.0F, 0.0F}));
  REQUIRE(std::ranges::equal(buffer.green.intensities(), std::array{0.0F, 1.0F, 0.0F, 0.0F, 0.0F}));
  REQUIRE(std::ranges::equal(buffer.blue.intensities(), std::array{0.0F, 1.0F, 0.0F, 0.0F, 0.0F}));
}

TEST_CASE("render: a single fully-lit white pixel", "[larson_scanner]") {
  larson_scanner::led_buffer buffer(1, 0.0F, 1.0F);
  buffer.tick(); // pins the one pixel to (1.0, 1.0, 1.0) on all three channels

  REQUIRE(larson_scanner::render(buffer) == "\x1b[38;2;255;255;255m█\x1b[0m");
}

TEST_CASE("render: an unlit pixel renders as blank, not the lightest block", "[larson_scanner]") {
  larson_scanner::led_buffer buffer(1, 0.0F, 1.0F);
  // never ticked - stays at intensity 0 on every channel

  REQUIRE(larson_scanner::render(buffer) == "\x1b[38;2;0;0;0m \x1b[0m");
}

TEST_CASE("render: a half-lit pixel picks the mid-height glyph and a scaled color",
          "[larson_scanner]") {
  larson_scanner::led_buffer buffer(2, 1.0F, 0.5F);
  buffer.tick(); // index 0 -> 1.0
  buffer.tick(); // index 0 decays to 0.5, index 1 pins to 1.0

  const std::string out = larson_scanner::render(buffer);
  REQUIRE(out.starts_with(
      "\x1b[38;2;127;127;127m▄")); // 0.5 * 255 = 127.5, truncated; level 4 of 8 -> ▄
}

TEST_CASE("parse_command: each channel/param spelling round-trips", "[larson_scanner]") {
  using larson_scanner::ChannelSelector;
  using larson_scanner::command;
  using larson_scanner::CommandKind;
  using larson_scanner::ParamKind;

  REQUIRE(larson_scanner::parse_command("r speed 0.5") ==
          command{.channel = ChannelSelector::red, .param = ParamKind::speed, .value = 0.5F});
  REQUIRE(larson_scanner::parse_command("g decay 0.92") ==
          command{.channel = ChannelSelector::green, .param = ParamKind::decay, .value = 0.92F});
  REQUIRE(larson_scanner::parse_command("b speed 1.2") ==
          command{.channel = ChannelSelector::blue, .param = ParamKind::speed, .value = 1.2F});
  REQUIRE(larson_scanner::parse_command("all decay 0.9") ==
          command{.channel = ChannelSelector::all, .param = ParamKind::decay, .value = 0.9F});
  REQUIRE(larson_scanner::parse_command("quit") == command{.kind = CommandKind::quit});
}

TEST_CASE("parse_command: rejects malformed input rather than guessing", "[larson_scanner]") {
  REQUIRE(larson_scanner::parse_command("bad input") == std::nullopt);         // wrong token count
  REQUIRE(larson_scanner::parse_command("r speed") == std::nullopt);           // missing value
  REQUIRE(larson_scanner::parse_command("r speed abc") == std::nullopt);       // non-numeric value
  REQUIRE(larson_scanner::parse_command("x speed 0.5") == std::nullopt);       // unknown channel
  REQUIRE(larson_scanner::parse_command("r foo 0.5") == std::nullopt);         // unknown param
  REQUIRE(larson_scanner::parse_command("r speed 0.5 extra") == std::nullopt); // trailing garbage
  REQUIRE(larson_scanner::parse_command("") == std::nullopt);
}

TEST_CASE("apply: a specific channel only touches that channel", "[larson_scanner]") {
  larson_scanner::led_buffer buffer(3, 1.0F, 1.0F);
  larson_scanner::apply(buffer,
                        {.channel = larson_scanner::ChannelSelector::red,
                         .param = larson_scanner::ParamKind::speed,
                         .value = 5.0F});

  REQUIRE(buffer.red.speed == 5.0F);
  REQUIRE(buffer.green.speed == 1.0F);
  REQUIRE(buffer.blue.speed == 1.0F);
  REQUIRE(buffer.red.decay == 1.0F); // the other param on the same channel is untouched
}

TEST_CASE("apply: green and blue each only touch their own channel too", "[larson_scanner]") {
  larson_scanner::led_buffer buffer(3, 1.0F, 1.0F);
  larson_scanner::apply(buffer,
                        {.channel = larson_scanner::ChannelSelector::green,
                         .param = larson_scanner::ParamKind::decay,
                         .value = 0.4F});
  larson_scanner::apply(buffer,
                        {.channel = larson_scanner::ChannelSelector::blue,
                         .param = larson_scanner::ParamKind::decay,
                         .value = 0.6F});

  REQUIRE(buffer.red.decay == 1.0F);
  REQUIRE(buffer.green.decay == 0.4F);
  REQUIRE(buffer.blue.decay == 0.6F);
}

TEST_CASE("apply: ChannelSelector::all touches every channel", "[larson_scanner]") {
  larson_scanner::led_buffer buffer(3, 1.0F, 1.0F);
  larson_scanner::apply(buffer,
                        {.channel = larson_scanner::ChannelSelector::all,
                         .param = larson_scanner::ParamKind::decay,
                         .value = 0.3F});

  REQUIRE(buffer.red.decay == 0.3F);
  REQUIRE(buffer.green.decay == 0.3F);
  REQUIRE(buffer.blue.decay == 0.3F);
}
