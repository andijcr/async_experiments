export module larson_scanner;

import std;

// The whole animator/renderer/protocol lives here as plain, synchronous
// logic - no est:: dependency at all, deliberately: nothing in this
// module needs a clock, a loop, or a coroutine. main.cpp is what wires
// scanner_channel::tick()/render()/parse_command()/apply() into
// est::schedule_periodic()/est::spsc_ring<T>/est::external_event<T>.
// Keeping this half free of that lets it be unit-tested directly
// (examples/multicolor_larson_scanner/tests/), matching
// examples/spreadsheet/src/spreadsheet.cppm's own split from
// spreadsheet_io.cppm for the identical reason.
export namespace larson_scanner {

// One color channel's own Larson ("KITT"/Cylon) scanner: a single point
// of full brightness bounces back and forth across a `width`-pixel
// strip, decaying every pixel by `decay` every second - the classic
// effect. `speed`/`decay` are public and mutable on purpose: the
// controller (main.cpp) adjusts them live, and a getter/setter pair
// would add ceremony without buying any real encapsulation - matching
// est::future_state<T>'s own state_/owner_ members (est/src/future.cppm)
// for the identical reason.
struct scanner_channel {
  // Always called positionally from led_buffer's own constructor below,
  // with the same argument order every time; a named-parameter redesign
  // would be ceremony this small, internal type doesn't need.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  explicit scanner_channel(std::size_t width, float initial_speed, float initial_decay)
      : speed(initial_speed), decay(initial_decay), intensity_(width, 0.0F) {}

  // One time step of `dt`: decay every pixel, pin the current position
  // to full brightness, then advance the position by `speed * dt` and
  // bounce off either end. Order matters - the position pinned to 1.0
  // is *this* tick's position, not the next one's, so a scanner sitting
  // still (speed == 0) keeps exactly one pixel lit rather than
  // drifting.
  //
  // Takes `dt` rather than assuming a fixed step per call so the motion
  // is genuinely time-independent: `speed` is pixels/second and `decay`
  // is a per-second falloff factor, not "per call" - calling tick() at
  // a different cadence (or with a jittered/irregular dt) still moves
  // and fades the scanner by the physically correct amount. The actual
  // schedule (how often tick() gets called) is entirely main.cpp's
  // concern; this function doesn't know or care.
  //
  // decay is applied as pow(decay, dt) rather than a plain `decay * dt`
  // multiply: exponential falloff compounds multiplicatively over time
  // (v(t) = v(0) * decay^t), so this is what keeps a given `decay`
  // value's *visual trail length in real time* constant regardless of
  // how often tick() happens to be called - two 0.5s ticks fade a pixel
  // by the same total factor as one 1.0s tick.
  void tick(std::chrono::duration<float> dt) noexcept {
    const float dt_seconds = dt.count();
    const float per_tick_decay = std::pow(decay, dt_seconds);
    for (float& v : intensity_) {
      v *= per_tick_decay;
    }
    // pinned is always < intensity_.size(): position_ starts at 0 and
    // every tick() call below clamps it back into [0, size-1] before
    // returning, so the *next* call's round() here can never leave that
    // range either - same "provably in range by construction" shape
    // est::spsc_ring<T>'s own unchecked accesses document
    // (est/src/sync/spsc_ring.cppm).
    const auto pinned = static_cast<std::size_t>(std::lround(position_));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    intensity_[pinned] = 1.0F;

    position_ += static_cast<float>(direction_) * speed * dt_seconds;
    const auto last_index = static_cast<float>(intensity_.size() - 1);
    if (position_ >= last_index) {
      position_ = last_index;
      direction_ = -1;
    } else if (position_ <= 0.0F) {
      position_ = 0.0F;
      direction_ = 1;
    }
  }

  [[nodiscard]] auto intensities() const noexcept -> std::span<const float> { return intensity_; }

  // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes)
  float speed; // pixels/second
  // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes)
  float decay; // multiplicative per-pixel falloff per *second*, in (0, 1) - see tick()'s own doc
               // comment

private:
  std::vector<float> intensity_;
  float position_ = 0.0F;
  int direction_ = 1; // +1 or -1, flips at either end of the strip
};

// The shared buffer three independent scanner_channels animate into -
// one per color. All three start at the same width/speed/decay; the
// controller (main.cpp) is what makes them diverge at runtime.
struct led_buffer {
  explicit led_buffer(std::size_t strip_width, float initial_speed, float initial_decay)
      : width(strip_width), red(strip_width, initial_speed, initial_decay),
        green(strip_width, initial_speed, initial_decay),
        blue(strip_width, initial_speed, initial_decay) {}

  void tick(std::chrono::duration<float> dt) noexcept {
    red.tick(dt);
    green.tick(dt);
    blue.tick(dt);
  }

  std::size_t width;
  scanner_channel red;
  scanner_channel green;
  scanner_channel blue;
};

// A width-long strip of Unicode block glyphs (one per pixel, height
// picked by that pixel's brightest channel) each wrapped in a 24-bit
// ("truecolor") ANSI foreground escape set from that pixel's own R/G/B
// intensities scaled to 0-255. Pure and independently testable - no
// terminal, no I/O, just a string.
[[nodiscard]] auto render(const led_buffer& buffer) -> std::string;

// Which scanner_channel(s) a command targets - `all` applies to every
// channel at once.
enum class ChannelSelector : std::uint8_t { red, green, blue, all };

// Which of scanner_channel's two live-tunable members a command sets.
enum class ParamKind : std::uint8_t { speed, decay };

enum class CommandKind : std::uint8_t { set_param, quit };

// Trivially copyable (plain enums + float) and default-constructible -
// satisfies est::spsc_ring<T>'s constraint trivially (est/src/sync/
// spsc_ring.cppm), the same as every other POD spsc_ring<T>
// instantiation in est's own tests. Produced by parse_command() below,
// consumed by apply().
struct command {
  CommandKind kind = CommandKind::set_param;
  ChannelSelector channel = ChannelSelector::all;
  ParamKind param = ParamKind::speed;
  float value = 0.0F;

  auto operator==(const command&) const -> bool = default;
};

// Parses one line of "<channel> <param> <value>" - channel in
// {r, g, b, all}, param in {speed, decay}, e.g. "r speed 0.5" or
// "all decay 0.92" - into a set_param command; the literal line "quit"
// into a quit command. std::nullopt for anything else unparseable
// (wrong token count, unknown channel/param, a non-numeric value,
// trailing garbage) - the caller decides how to report that, this
// function doesn't print anything.
[[nodiscard]] auto parse_command(std::string_view line) -> std::optional<command>;

// Applies a set_param command to the matching scanner_channel(s) of
// buffer - ChannelSelector::all touches all three, otherwise just the
// one named. Precondition: cmd.kind == CommandKind::set_param (a quit
// command is main.cpp's own concern, not this module's - see
// drain_commands() there).
void apply(led_buffer& buffer, const command& cmd) noexcept;

} // namespace larson_scanner

namespace {

// 9 levels (0 = blank, 8 = full block) - one more level than a typical
// "8 eighths" reading because level 0 needs its own glyph (a space,
// not the lightest block) for a genuinely unlit pixel to render as
// empty rather than always showing at least a sliver.
constexpr std::array<std::string_view, 9> glyphs = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};

[[nodiscard]] auto scale_to_byte(float intensity) noexcept -> int {
  return static_cast<int>(std::clamp(intensity, 0.0F, 1.0F) * 255.0F);
}

[[nodiscard]] auto glyph_for(float intensity) -> std::string_view {
  const auto level =
      static_cast<std::size_t>(std::lround(std::clamp(intensity, 0.0F, 1.0F) * 8.0F));
  return glyphs.at(level);
}

} // namespace

namespace larson_scanner {

auto render(const led_buffer& buffer) -> std::string {
  std::string out;
  for (auto [r, g, b] : std::views::zip(
           buffer.red.intensities(), buffer.green.intensities(), buffer.blue.intensities())) {
    const float level = std::max({r, g, b});
    out += std::format("\x1b[38;2;{};{};{}m{}",
                       scale_to_byte(r),
                       scale_to_byte(g),
                       scale_to_byte(b),
                       glyph_for(level));
  }
  out += "\x1b[0m";
  return out;
}

auto parse_command(std::string_view line) -> std::optional<command> {
  if (line == "quit") {
    return command{.kind = CommandKind::quit};
  }

  std::istringstream stream{std::string(line)};
  std::string channel_token;
  std::string param_token;
  float value = 0.0F;
  if (!(stream >> channel_token >> param_token >> value)) {
    return std::nullopt;
  }
  std::string trailing;
  if (stream >> trailing) {
    return std::nullopt; // extra tokens after value - reject rather than silently ignore
  }

  ChannelSelector channel{};
  if (channel_token == "r") {
    channel = ChannelSelector::red;
  } else if (channel_token == "g") {
    channel = ChannelSelector::green;
  } else if (channel_token == "b") {
    channel = ChannelSelector::blue;
  } else if (channel_token == "all") {
    channel = ChannelSelector::all;
  } else {
    return std::nullopt;
  }

  ParamKind param{};
  if (param_token == "speed") {
    param = ParamKind::speed;
  } else if (param_token == "decay") {
    param = ParamKind::decay;
  } else {
    return std::nullopt;
  }

  return command{
      .kind = CommandKind::set_param, .channel = channel, .param = param, .value = value};
}

void apply(led_buffer& buffer, const command& cmd) noexcept {
  const auto set = [&](scanner_channel& ch) noexcept {
    if (cmd.param == ParamKind::speed) {
      ch.speed = cmd.value;
    } else {
      ch.decay = cmd.value;
    }
  };
  switch (cmd.channel) {
    case ChannelSelector::red:
      set(buffer.red);
      break;
    case ChannelSelector::green:
      set(buffer.green);
      break;
    case ChannelSelector::blue:
      set(buffer.blue);
      break;
    case ChannelSelector::all:
      set(buffer.red);
      set(buffer.green);
      set(buffer.blue);
      break;
  }
}

} // namespace larson_scanner
