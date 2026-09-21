export module est:util.jitter;

import std;
import :check;
import :platform;

export namespace est {

// Adds bounded, uniformly distributed jitter to a duration - so several
// independent periodic sources sharing one loop (est::schedule_periodic(),
// :timer.periodic, the one caller today) don't all wake in lockstep.
// Seeds itself once, at construction, from
// platform::instance().get_random_seed() (:platform) - the one place in
// this codebase that needs actual randomness. Nothing here is
// cryptographically secure, nor does it need to be: jitter only needs to
// differ from the last draw, never to resist prediction.
//
// std::minstd_rand (a 32-bit Lehmer/Park-Miller LCG), not std::mt19937:
// single-word state (a handful of bytes, vs. mt19937's ~2.5KB) is plenty
// for spreading out wakeups - this codebase's eventual bare-metal target
// has no reason to carry a much larger generator for a job this
// undemanding.
class jitter {
public:
  using duration = platform::clock::duration;

  // max_jitter must be non-negative - checked. A zero max_jitter is a
  // legitimate, if pointless, way to say "no jitter": every draw is then
  // exactly zero.
  explicit jitter(duration max_jitter) noexcept
      : engine_(seed_from(platform::instance().get_random_seed())),
        dist_(-clamp(max_jitter).count(), clamp(max_jitter).count()) {
    check(max_jitter >= duration::zero(), "est::jitter: max_jitter must be non-negative");
  }

  // A fresh draw, uniformly distributed in [-max_jitter, +max_jitter],
  // every call.
  [[nodiscard]] auto operator()() -> duration { return duration(dist_(engine_)); }

private:
  static auto clamp(duration d) noexcept -> duration { return std::max(d, duration::zero()); }

  // XOR-folds get_random_seed()'s full 64 bits down to minstd_rand's own
  // 32-bit seed, rather than a plain truncating cast that would silently
  // discard the upper half - on hosted_stdcpp specifically, the entire
  // first of its two std::random_device draws - for nothing.
  static auto seed_from(std::uint64_t seed) noexcept -> std::minstd_rand::result_type {
    return static_cast<std::minstd_rand::result_type>(seed ^ (seed >> 32));
  }

  std::minstd_rand engine_;
  std::uniform_int_distribution<duration::rep> dist_;
};

} // namespace est
