// Note: this should be run with --compilation_mode=opt, on a quiescent system, with
// STATS_BENCH_CPUS set (see bench_pinning.h for placement, thread counts, timing and counters).
//
// Multi-threaded stat update benchmarks on real stat objects, created through a
// ThreadLocalStoreImpl scope so the file compiles on every arm. N threads update:
//   * one shared stat (cache-line transfer plus the cost of the atomic operations);
//   * one stat each, created back to back as a stats struct creates them, which can put
//     neighbours on one cache line or prefetch pair (false sharing); the smallest address
//     distance between two threads' stats is reported as `min_distance_bytes`;
//   * one stat each, spaced: allocated until verified, so every pair of threads' stats is at
//     least 256 bytes apart (rejected candidates stay alive as spacers; primitive stats sit in
//     256-byte-aligned wrappers). This is the
//     no-sharing floor, which leaves only the cost of the atomic operations.
// bmRequestMix* runs, per iteration, the stat operations of one proxied request, given by
// STATS_BENCH_MIX="<counter incs>:<gauge inc/dec pairs>:<gauge sets>:<primitive counter incs>:
// <primitive gauge inc/dec pairs>:<histogram samples>" (counts come from a running proxy). There
// is no default: without the variable the mix benchmarks skip. Histogram samples always go to the
// thread's own ThreadLocalHistogramImpl, as on a worker.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "envoy/stats/primitive_stats.h"

#include "source/common/stats/allocator.h"
#include "source/common/stats/symbol_table.h"
#include "source/common/stats/thread_local_store.h"

#include "test/common/stats/bench_pinning.h"

#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "benchmark/benchmark.h"

namespace Envoy {
namespace Stats {
namespace {

using BenchPinning::distance;
using BenchPinning::MinSpacedDistance;

// In the spaced layout a candidate stat closer than MinSpacedDistance to any stat already chosen
// for another thread is kept alive as a spacer and another one is created, up to this many
// candidates per stat. The allocator decides where stats land, so a fixed number of spacers
// between them does not guarantee the distance.
constexpr size_t MaxSpacingAttempts = 256;

// Each wrapper fills a whole MinSpacedDistance region, so consecutive allocations of them are
// MinSpacedDistance apart and pass the spacing check.
struct alignas(BenchPinning::MinSpacedDistance) PaddedPrimitiveCounter {
  PrimitiveCounter value;
};
struct alignas(BenchPinning::MinSpacedDistance) PaddedPrimitiveGauge {
  PrimitiveGauge value;
};

struct RequestMix {
  size_t counters = 0;
  size_t gauge_pairs = 0;
  size_t gauge_sets = 0;
  size_t primitive_counters = 0;
  size_t primitive_gauge_pairs = 0;
  size_t histogram_samples = 0;
};

// Parses STATS_BENCH_MIX; nullopt when unset or malformed.
[[maybe_unused]] std::optional<RequestMix> requestMix() {
  const char* env = std::getenv("STATS_BENCH_MIX");
  if (env == nullptr) {
    return std::nullopt;
  }
  std::vector<absl::string_view> parts = absl::StrSplit(env, ':');
  if (parts.size() != 6) {
    return std::nullopt;
  }
  RequestMix mix;
  size_t* fields[] = {&mix.counters,           &mix.gauge_pairs,
                      &mix.gauge_sets,         &mix.primitive_counters,
                      &mix.primitive_gauge_pairs, &mix.histogram_samples};
  static_assert(sizeof(fields) / sizeof(fields[0]) == 6);
  for (size_t i = 0; i < 6; ++i) {
    if (!absl::SimpleAtoi(parts[i], fields[i])) {
      return std::nullopt;
    }
  }
  return mix;
}

// The stats one thread (or every thread, when shared) updates.
struct StatSet {
  std::vector<Counter*> counters;
  std::vector<Gauge*> gauge_pairs;
  std::vector<Gauge*> gauge_sets;
  std::vector<PrimitiveCounter*> primitive_counters;
  std::vector<PrimitiveGauge*> primitive_gauges;
};

class StatUpdateFixture {
public:
  StatUpdateFixture()
      : pool_(symbol_table_), alloc_(symbol_table_), store_(alloc_),
        histogram_name_(pool_.add("thread_histogram")) {}

  void build(size_t num_sets, const RequestMix& mix, bool spaced) {
    for (size_t s = 0; s < num_sets; ++s) {
      StatSet set;
      for (size_t i = 0; i < mix.counters; ++i) {
        set.counters.push_back(place(spaced, [this]() -> Counter* { return &counter(); }));
      }
      for (size_t i = 0; i < mix.gauge_pairs + mix.gauge_sets; ++i) {
        (i < mix.gauge_pairs ? set.gauge_pairs : set.gauge_sets)
            .push_back(place(spaced, [this]() -> Gauge* { return &gauge(); }));
      }
      for (size_t i = 0; i < mix.primitive_counters; ++i) {
        primitive_counters_.push_back(std::make_unique<PaddedPrimitiveCounter>());
        set.primitive_counters.push_back(&primitive_counters_.back()->value);
      }
      for (size_t i = 0; i < mix.primitive_gauge_pairs; ++i) {
        primitive_gauges_.push_back(std::make_unique<PaddedPrimitiveGauge>());
        set.primitive_gauges.push_back(&primitive_gauges_.back()->value);
      }
      sets_.push_back(std::move(set));
    }
  }

  // False when a spaced stat could not be placed within MaxSpacingAttempts candidates.
  bool spacingOk() const { return spacing_ok_; }
  // The largest number of candidates one spaced stat needed.
  size_t spacingAttempts() const { return spacing_attempts_; }

  // Smallest address distance between any stat of one set and any stat of another.
  uintptr_t minDistance() const {
    std::vector<std::vector<const void*>> addresses;
    for (const StatSet& set : sets_) {
      addresses.push_back(StatUpdateFixture::addresses(set));
    }
    uintptr_t min = UINTPTR_MAX;
    for (size_t x = 0; x < addresses.size(); ++x) {
      for (size_t y = x + 1; y < addresses.size(); ++y) {
        for (const void* p : addresses[x]) {
          for (const void* q : addresses[y]) {
            min = std::min(min, distance(p, q));
          }
        }
      }
    }
    return min;
  }

  StatSet& set(size_t i) { return sets_[i]; }
  size_t numSets() const { return sets_.size(); }
  SymbolTable& symbolTable() { return symbol_table_; }
  StatName histogramName() const { return histogram_name_; }
  BenchPinning::RunSync& sync() { return sync_; }

private:
  // Allocate until verified: creates stats with `make` until one is at least MinSpacedDistance
  // from every stat already chosen for another thread. Rejected candidates stay alive in the store
  // as spacers.
  template <class Make> auto place(bool spaced, Make make) -> decltype(make()) {
    auto stat = make();
    if (!spaced) {
      return stat;
    }
    size_t attempts = 1;
    while (tooCloseToOtherSets(stat)) {
      if (attempts >= MaxSpacingAttempts) {
        spacing_ok_ = false;
        break;
      }
      stat = make();
      ++attempts;
    }
    spacing_attempts_ = std::max(spacing_attempts_, attempts);
    return stat;
  }

  bool tooCloseToOtherSets(const void* p) const {
    for (const StatSet& set : sets_) {
      for (const void* q : addresses(set)) {
        if (distance(p, q) < MinSpacedDistance) {
          return true;
        }
      }
    }
    return false;
  }

  static std::vector<const void*> addresses(const StatSet& set) {
    std::vector<const void*> a(set.counters.begin(), set.counters.end());
    a.insert(a.end(), set.gauge_pairs.begin(), set.gauge_pairs.end());
    a.insert(a.end(), set.gauge_sets.begin(), set.gauge_sets.end());
    a.insert(a.end(), set.primitive_counters.begin(), set.primitive_counters.end());
    a.insert(a.end(), set.primitive_gauges.begin(), set.primitive_gauges.end());
    return a;
  }

  Counter& counter() {
    return store_.rootScope()->counterFromStatName(pool_.add(absl::StrCat("c.", next_++)));
  }
  Gauge& gauge() {
    return store_.rootScope()->gaugeFromStatName(pool_.add(absl::StrCat("g.", next_++)),
                                                 Gauge::ImportMode::Accumulate);
  }

  SymbolTableImpl symbol_table_;
  StatNamePool pool_;
  Allocator alloc_;
  ThreadLocalStoreImpl store_;
  const StatName histogram_name_;
  uint64_t next_ = 0;
  std::vector<StatSet> sets_;
  bool spacing_ok_ = true;
  size_t spacing_attempts_ = 0;
  std::vector<std::unique_ptr<PaddedPrimitiveCounter>> primitive_counters_;
  std::vector<std::unique_ptr<PaddedPrimitiveGauge>> primitive_gauges_;
  BenchPinning::RunSync sync_;
};

std::optional<StatUpdateFixture> fixture;
std::string setup_error;

enum class Layout { Shared, PerThread, PerThreadSpaced };
enum class Op { CounterInc, GaugeIncDec, GaugeSet, PrimitiveCounterInc, Mix };

RequestMix singleOpMix(Op op) {
  RequestMix mix;
  switch (op) {
  case Op::CounterInc:
    mix.counters = 1;
    break;
  case Op::GaugeIncDec:
    mix.gauge_pairs = 1;
    break;
  case Op::GaugeSet:
    mix.gauge_sets = 1;
    break;
  case Op::PrimitiveCounterInc:
    mix.primitive_counters = 1;
    break;
  case Op::Mix:
    break;
  }
  return mix;
}

template <Layout layout, Op op> void setup(const ::benchmark::State& state) {
  setup_error.clear();
  fixture.emplace();
  fixture->sync().reset(state.threads());
  RequestMix mix = singleOpMix(op);
  if (op == Op::Mix) {
    std::optional<RequestMix> env_mix = requestMix();
    if (!env_mix.has_value()) {
      setup_error = "STATS_BENCH_MIX unset or malformed (six ':'-separated counts)";
      return;
    }
    mix = *env_mix;
  }
  fixture->build(layout == Layout::Shared ? 1 : state.threads(), mix,
                 layout == Layout::PerThreadSpaced);
  if (layout == Layout::PerThreadSpaced && !fixture->spacingOk()) {
    setup_error = absl::StrCat("could not place spaced stats 256 bytes apart within ",
                               MaxSpacingAttempts, " candidates");
  } else if (layout == Layout::PerThreadSpaced && fixture->numSets() > 1 &&
             fixture->minDistance() < MinSpacedDistance) {
    setup_error = "spaced stats are closer than 256 bytes";
  }
}

void teardown(const ::benchmark::State&) { fixture.reset(); }

template <Layout layout, Op op> void body(::benchmark::State& state) {
  if (!setup_error.empty()) {
    if (state.thread_index() == 0 && fixture->numSets() > 1) {
      state.counters["spacing_attempts"] = static_cast<double>(fixture->spacingAttempts());
      state.counters["spacing_ok"] = fixture->spacingOk() ? 1 : 0;
    }
    // Every thread enters the loop so the run ends cleanly; the run is reported as skipped.
    state.SkipWithError(setup_error.c_str());
    for (auto _ : state) { // NOLINT
    }
    return;
  }
  StatSet& set = fixture->set(layout == Layout::Shared ? 0 : state.thread_index());
  if (state.thread_index() == 0 && fixture->numSets() > 1) {
    // The achieved distance, and how many candidates the hardest-placed spaced stat needed.
    state.counters["min_distance_bytes"] = static_cast<double>(fixture->minDistance());
    state.counters["spacing_attempts"] = static_cast<double>(fixture->spacingAttempts());
  }

  // The thread's own TLS histogram, created on this thread as a worker creates its own.
  std::unique_ptr<ThreadLocalHistogramImpl> histogram;
  size_t histogram_samples = 0;
  if (op == Op::Mix) {
    histogram = std::make_unique<ThreadLocalHistogramImpl>(
        fixture->histogramName(), Histogram::Unit::Unspecified, fixture->histogramName(),
        StatNameTagSpan(), fixture->symbolTable(), std::nullopt);
    histogram_samples = requestMix()->histogram_samples;
  }
  uint64_t sample = 0;

  BenchPinning::timedLoop(state, fixture->sync(), 0, [&]() {
    for (Gauge* gauge : set.gauge_pairs) {
      gauge->inc();
    }
    for (PrimitiveGauge* gauge : set.primitive_gauges) {
      gauge->inc();
    }
    for (Counter* counter : set.counters) {
      counter->inc();
    }
    for (PrimitiveCounter* counter : set.primitive_counters) {
      counter->inc();
    }
    for (Gauge* gauge : set.gauge_sets) {
      gauge->set(++sample);
    }
    for (size_t i = 0; i < histogram_samples; ++i) {
      histogram->recordValue(++sample & 0xffff);
    }
    for (PrimitiveGauge* gauge : set.primitive_gauges) {
      gauge->dec();
    }
    for (Gauge* gauge : set.gauge_pairs) {
      gauge->dec();
    }
  });
}

#define STAT_UPDATE_BENCHMARK(name, layout, op)                                                    \
  void name(::benchmark::State& state) { body<layout, op>(state); }                               \
  BENCHMARK(name)                                                                                  \
      ->Apply(BenchPinning::multiThreaded)                                                         \
      ->Setup(setup<layout, op>)                                                                   \
      ->Teardown(teardown)

STAT_UPDATE_BENCHMARK(bmCounterIncShared, Layout::Shared, Op::CounterInc);
STAT_UPDATE_BENCHMARK(bmCounterIncPerThread, Layout::PerThread, Op::CounterInc);
STAT_UPDATE_BENCHMARK(bmCounterIncPerThreadSpaced, Layout::PerThreadSpaced, Op::CounterInc);
STAT_UPDATE_BENCHMARK(bmGaugeIncDecShared, Layout::Shared, Op::GaugeIncDec);
STAT_UPDATE_BENCHMARK(bmGaugeIncDecPerThreadSpaced, Layout::PerThreadSpaced, Op::GaugeIncDec);
STAT_UPDATE_BENCHMARK(bmGaugeSetShared, Layout::Shared, Op::GaugeSet);
STAT_UPDATE_BENCHMARK(bmGaugeSetPerThreadSpaced, Layout::PerThreadSpaced, Op::GaugeSet);
STAT_UPDATE_BENCHMARK(bmPrimitiveCounterIncShared, Layout::Shared, Op::PrimitiveCounterInc);
STAT_UPDATE_BENCHMARK(bmPrimitiveCounterIncPerThreadSpaced, Layout::PerThreadSpaced,
                      Op::PrimitiveCounterInc);
// The request-mix benchmarks (Op::Mix, STATS_BENCH_MIX) are not registered: the narrowed run
// set drops them. Re-register bmRequestMixShared / bmRequestMixPerThreadSpaced to use them.

} // namespace
} // namespace Stats
} // namespace Envoy
