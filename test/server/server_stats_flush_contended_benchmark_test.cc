// Note: this should be run with --compilation_mode=opt, on a quiescent system, with
// STATS_BENCH_CPUS set (see test/common/stats/bench_pinning.h): the benchmark thread, which
// flushes, takes the first CPU and the K hammer threads the next K.
//
// The cost of a stats flush that overlaps worker updates. A store holds S stats of each type; the
// benchmark thread flushes it to a sink that reads every value, while K hammer threads keep
// updating stats, in one of three modes:
//   0  a shared hot set of 64 counters and gauges (the size of one cluster's traffic stats)
//   1  each hammer walks its own slice of all S counters and gauges: this measures memory
//      traffic, not atomics, and is labelled so
//   2  each hammer updates a private hot set of 8 counters and gauges, spaced apart: the mode
//      that can show an ordering effect
// The flush is duty-cycled: after each flush the benchmark thread idles for as long as that flush
// took (at least 1 ms), and the hammer counts are bucketed by phase.
// Reported, besides the flush time per iteration:
//   hammer_rate_during   hammer updates per second while a flush is in progress
//   hammer_rate_between  hammer updates per second between flushes (a fixed idle gap)
// bmHammerOnly runs the hammers with no flush at all, as the baseline for both rates. Hammer
// counts come from STATS_BENCH_HAMMERS (bench_pinning.h); K hammers plus the flushing thread
// need K + 1 CPUs in STATS_BENCH_CPUS.
//
// The source only uses APIs present on every arm, so it compiles unchanged on each.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <list>
#include <memory>
#include <vector>

#include "envoy/stats/sink.h"
#include "envoy/stats/stats.h"

#include "source/common/common/thread.h"
#include "source/common/stats/thread_local_store.h"
#include "source/server/server.h"

#include "test/benchmark/main.h"
#include "test/common/stats/bench_pinning.h"
#include "test/mocks/upstream/cluster_manager.h"
#include "test/test_common/simulated_time_system.h"
#include "test/test_common/test_time.h"
#include "test/test_common/thread_factory_for_test.h"

#include "absl/strings/str_cat.h"
#include "absl/synchronization/notification.h"
#include "benchmark/benchmark.h"
#include "gmock/gmock.h"

namespace Envoy {
namespace {

constexpr size_t SharedHotSetSize = 64;
constexpr size_t PrivateHotSetSize = 8;
// Distance, in stats, between two members of a private hot set.
constexpr size_t PrivateHotSetStride = 8;
constexpr std::chrono::milliseconds IdleGap{10};
constexpr std::chrono::milliseconds MinFlushIdle{1};

const char* modeName(int mode) {
  switch (mode) {
  case 0:
    return "shared-hot";
  case 1:
    return "walk(memory-traffic)";
  default:
    return "private-hot";
  }
}

class FastMockClusterManager : public testing::StrictMock<Upstream::MockClusterManager> {
public:
  ClusterInfoMaps clusters() const override { return ClusterInfoMaps{}; }
  void forEachActiveCluster(std::function<void(const Upstream::Cluster&)>) const override {}
};

// A sink that reads every value in the snapshot.
class ReadAllSink : public Stats::Sink {
public:
  void flush(Stats::MetricSnapshot& snapshot) override {
    uint64_t sum = 0;
    for (const auto& counter : snapshot.counters()) {
      sum += counter.delta_ + counter.counter_.get().value();
    }
    for (const auto& gauge : snapshot.gauges()) {
      sum += gauge.get().value();
    }
    for (const auto& text_readout : snapshot.textReadouts()) {
      sum += text_readout.get().value().size();
    }
    for (const auto& histogram : snapshot.histograms()) {
      sum += histogram.get().used() ? 1 : 0;
    }
    ::benchmark::DoNotOptimize(sum);
  }
  void onHistogramComplete(const Stats::Histogram&, uint64_t) override {}
};

// Per-hammer update count, on its own cache lines so the bookkeeping does not contend.
struct alignas(128) HammerSlot {
  std::atomic<uint64_t> updates{0};
};

class ContendedFlushTest {
public:
  explicit ContendedFlushTest(size_t num_stats)
      : pool_(symbol_table_), alloc_(symbol_table_), store_(alloc_) {
    Stats::Scope& scope = *store_.rootScope();
    for (size_t i = 0; i < num_stats; ++i) {
      counters_.push_back(&scope.counterFromStatName(pool_.add(absl::StrCat("counter.", i))));
      gauges_.push_back(&scope.gaugeFromStatName(pool_.add(absl::StrCat("gauge.", i)),
                                                 Stats::Gauge::ImportMode::NeverImport));
      scope.textReadoutFromStatName(pool_.add(absl::StrCat("text_readout.", i)))
          .set(absl::StrCat("text_readout.", i));
      store_.histogramFromString(absl::StrCat("histogram.", i),
                                 Stats::Histogram::Unit::Unspecified);
    }
    sinks_.emplace_back(new ReadAllSink());
  }

  // Runs the timed loop with `num_hammers` hammer threads. With `flush` false the timed loop only
  // waits, so the hammer rates form the no-flush baseline.
  void run(::benchmark::State& state, size_t num_hammers, int mode, bool flush) {
    // The benchmark thread is the caller's thread: pin it for this run only.
    Stats::BenchPinning::ScopedPin pin(0);
    if (!pin.error().empty()) {
      state.SkipWithError(pin.error().c_str());
      return;
    }
    if (counters_.size() != gauges_.size()) {
      state.SkipWithError("counter and gauge counts differ");
      return;
    }
    if (mode == 2 && counters_.size() < num_hammers * PrivateHotSetSize * PrivateHotSetStride) {
      state.SkipWithError("too few stats for the private hot sets");
      return;
    }
    state.SetLabel(absl::StrCat(Stats::BenchPinning::label(), " mode=", modeName(mode),
                                flush ? " flush" : " hammer-only"));

    std::vector<HammerSlot> slots(num_hammers);
    std::atomic<bool> stop{false};
    absl::Notification go;
    std::vector<Thread::ThreadPtr> hammers;
    std::atomic<bool> pin_failed{false};
    for (size_t t = 0; t < num_hammers; ++t) {
      hammers.push_back(Thread::threadFactoryForTest().createThread([&, t]() {
        if (!Stats::BenchPinning::pinOwnThread(t + 1).empty()) {
          pin_failed.store(true);
        }
        const std::vector<size_t> set = statsFor(t, num_hammers, mode);
        go.WaitForNotification();
        uint64_t local = 0;
        while (!stop.load(std::memory_order_relaxed)) {
          for (size_t i : set) {
            counters_[i]->inc();
            gauges_[i]->inc();
            gauges_[i]->dec();
          }
          local += 3 * set.size();
          slots[t].updates.store(local, std::memory_order_relaxed);
        }
      }));
    }

    auto total = [&slots]() {
      uint64_t sum = 0;
      for (const HammerSlot& slot : slots) {
        sum += slot.updates.load(std::memory_order_relaxed);
      }
      return sum;
    };

    double during_updates = 0;
    double during_seconds = 0;
    double between_updates = 0;
    double between_seconds = 0;
    go.Notify();
    for (auto _ : state) { // NOLINT
      const MonotonicTime t0 = time_.monotonicTime();
      const uint64_t c0 = total();
      if (flush) {
        Server::InstanceUtil::flushMetricsToSinks(sinks_, store_, cm_, simulated_time_);
      } else {
        spinUntil(t0 + IdleGap);
      }
      const MonotonicTime t1 = time_.monotonicTime();
      const uint64_t c1 = total();
      during_updates += c1 - c0;
      during_seconds += seconds(t1 - t0);

      // Duty cycle: idle for as long as the flush took, so the "between" phase is comparable.
      const auto idle = flush ? std::max<std::chrono::nanoseconds>(t1 - t0, MinFlushIdle)
                              : std::chrono::nanoseconds(IdleGap);
      state.PauseTiming();
      spinUntil(t1 + idle);
      const MonotonicTime t2 = time_.monotonicTime();
      between_updates += total() - c1;
      between_seconds += seconds(t2 - t1);
      state.ResumeTiming();
    }
    stop.store(true);
    for (auto& hammer : hammers) {
      hammer->join();
    }
    if (pin_failed.load()) {
      state.SkipWithError("could not pin every hammer thread");
      return;
    }
    state.SetItemsProcessed(state.iterations() * counters_.size());
    if (num_hammers > 0) {
      state.counters["hammer_rate_during"] =
          during_seconds > 0 ? during_updates / during_seconds : 0;
      state.counters["hammer_rate_between"] =
          between_seconds > 0 ? between_updates / between_seconds : 0;
    }
  }

private:
  // The stats hammer t updates in the given mode.
  std::vector<size_t> statsFor(size_t t, size_t num_hammers, int mode) const {
    std::vector<size_t> set;
    const size_t n = counters_.size();
    if (mode == 0) {
      for (size_t i = 0; i < std::min(SharedHotSetSize, n); ++i) {
        set.push_back(i);
      }
    } else if (mode == 1) {
      for (size_t i = n * t / num_hammers; i < n * (t + 1) / num_hammers; ++i) {
        set.push_back(i);
      }
    } else {
      const size_t base = t * PrivateHotSetSize * PrivateHotSetStride;
      for (size_t j = 0; j < PrivateHotSetSize; ++j) {
        set.push_back(base + j * PrivateHotSetStride);
      }
    }
    return set;
  }

  void spinUntil(MonotonicTime deadline) {
    while (time_.monotonicTime() < deadline) {
    }
  }

  static double seconds(std::chrono::nanoseconds d) {
    return std::chrono::duration<double>(d).count();
  }

  Stats::SymbolTableImpl symbol_table_;
  Stats::StatNamePool pool_;
  Stats::Allocator alloc_;
  Stats::ThreadLocalStoreImpl store_;
  std::vector<Stats::Counter*> counters_;
  std::vector<Stats::Gauge*> gauges_;
  std::list<Stats::SinkPtr> sinks_;
  Event::SimulatedTimeSystem simulated_time_;
  Event::TestRealTimeSystem time_;
  FastMockClusterManager cm_;
};

bool skipExpensive(::benchmark::State& state) {
  if (Envoy::benchmark::skipExpensiveBenchmarks() && state.range(0) > 10000) {
    state.SkipWithError("Skipping expensive benchmark");
    return true;
  }
  return false;
}

// Arguments: stats per type S, hammer threads K, mode.
void bmFlushContended(::benchmark::State& state) {
  if (skipExpensive(state)) {
    return;
  }
  ContendedFlushTest test(state.range(0));
  test.run(state, state.range(1), state.range(2), /*flush=*/true);
}

void bmHammerOnly(::benchmark::State& state) {
  if (skipExpensive(state)) {
    return;
  }
  ContendedFlushTest test(state.range(0));
  test.run(state, state.range(1), state.range(2), /*flush=*/false);
}

void flushArgs(::benchmark::internal::Benchmark* b, bool with_zero_hammers) {
  std::vector<int64_t> hammers;
  for (int k : Stats::BenchPinning::hammerCounts()) {
    if (k > 0 || with_zero_hammers) {
      hammers.push_back(k);
    }
  }
  b->ArgsProduct({{10000, 100000, 1000000}, hammers, {0, 1, 2}});
}

BENCHMARK(bmFlushContended)
    ->Unit(::benchmark::kMillisecond)
    ->UseRealTime()
    ->Apply([](::benchmark::internal::Benchmark* b) { flushArgs(b, true); });
BENCHMARK(bmHammerOnly)
    ->Unit(::benchmark::kMillisecond)
    ->UseRealTime()
    ->Apply([](::benchmark::internal::Benchmark* b) { flushArgs(b, false); });

} // namespace
} // namespace Envoy
