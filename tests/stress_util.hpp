// Shared helpers for the randomized stress tests (deque_stress_test.cpp,
// pool_stress_test.cpp).
//
// Reproducibility: every random decision is derived from one base seed. The default is
// fixed so a plain `ctest` run is deterministic in what it attempts (thread interleavings
// still vary, which is the point). Two environment variables control a run:
//
//   STEALPOOL_STRESS_SEED   base seed (decimal or 0x-prefixed hex). Every test prints the
//                           base seed and the derived per-configuration seeds, and every
//                           failure message names them, so a failing configuration can be
//                           rerun with exactly the same random choices.
//   STEALPOOL_STRESS_SCALE  multiplier for the amount of work (default 1, e.g. 0.1 for a
//                           quick smoke run or 10 for a long soak).
//
// Every test prints the operation totals it executed, so "millions of operations" is a
// number in the test log rather than a claim.
#pragma once

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace stress {

inline constexpr std::uint64_t kDefaultSeed = 0x5eed2026;

inline std::uint64_t Mix(std::uint64_t x) noexcept {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

// Small, fast, platform-independent generator. std::mt19937_64 would also be
// reproducible, but the standard distributions are not identical across standard
// libraries, so ranges are reduced with plain arithmetic instead.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) noexcept : state_(Mix(seed)) {}

  std::uint64_t next() noexcept {
    state_ += 0x9e3779b97f4a7c15ULL;  // splitmix64 step
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }

  // Uniform enough in [0, bound) for test decisions; bound must be non-zero.
  std::uint64_t below(std::uint64_t bound) noexcept { return next() % bound; }

  // True with probability percent / 100.
  bool chance(unsigned percent) noexcept { return below(100) < percent; }

 private:
  std::uint64_t state_;
};

inline std::uint64_t BaseSeed() {
  static const std::uint64_t seed = [] {
    const char* text = std::getenv("STEALPOOL_STRESS_SEED");
    if (text == nullptr || *text == '\0') return kDefaultSeed;
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
      std::fprintf(stderr, "ignoring malformed STEALPOOL_STRESS_SEED='%s'\n", text);
      return kDefaultSeed;
    }
    return static_cast<std::uint64_t>(value);
  }();
  return seed;
}

inline double Scale() {
  static const double scale = [] {
    const char* text = std::getenv("STEALPOOL_STRESS_SCALE");
    if (text == nullptr || *text == '\0') return 1.0;
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(value) || value <= 0) {
      std::fprintf(stderr, "ignoring malformed STEALPOOL_STRESS_SCALE='%s'\n", text);
      return 1.0;
    }
    return value;
  }();
  return scale;
}

// `base` units of work adjusted by STEALPOOL_STRESS_SCALE, never less than `minimum`.
inline std::uint64_t Scaled(std::uint64_t base, std::uint64_t minimum = 1) {
  const double value = std::round(static_cast<double>(base) * Scale());
  return std::max<std::uint64_t>(minimum, static_cast<std::uint64_t>(value));
}

// Seed for configuration `index` of test `salt`, derived from the base seed.
inline std::uint64_t DerivedSeed(std::uint64_t salt, std::uint64_t index) {
  return Mix(BaseSeed() ^ Mix(salt * 0x100000001b3ULL + index));
}

inline std::string Hex(std::uint64_t value) {
  char buffer[24];
  std::snprintf(buffer, sizeof buffer, "0x%llx", static_cast<unsigned long long>(value));
  return buffer;
}

// Text attached to every assertion of one configuration (via SCOPED_TRACE) so a failure
// says how to reproduce it.
inline std::string ReproText(std::uint64_t seed, const std::string& config) {
  return "derived seed " + Hex(seed) + ", " + config + "; reproduce with STEALPOOL_STRESS_SEED=" +
         Hex(BaseSeed()) + " STEALPOOL_STRESS_SCALE=" + std::to_string(Scale());
}

// Printed once at the start of each test.
inline void PrintHeader(const char* test) {
  std::printf("[stress] %s: base seed %s, scale %g\n", test, Hex(BaseSeed()).c_str(), Scale());
  std::fflush(stdout);
}

inline unsigned long long U(std::uint64_t value) { return static_cast<unsigned long long>(value); }

// Spins without timing assumptions: yields so that oversubscribed runs (sanitizers, a
// shared machine) still make progress.
template <class Pred>
void SpinUntil(Pred&& pred) {
  while (!pred()) std::this_thread::yield();
}

}  // namespace stress
