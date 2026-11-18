// Note: this should be run with --compilation_mode=opt, on a quiescent system, with
// STATS_BENCH_CPUS set (see bench_pinning.h).
//
// Models of a counter's increment, with no Envoy stat classes, so one binary holds every variant
// and build-to-build layout differences drop out of the ordering question. ModelCounter has the
// same hot fields, in the same order and with the same sizes, as CounterImpl
// (source/common/stats/allocator.cc): ref_count_, flags_, value_, pending_increment_.
//
//   Current        three seq_cst read-modify-writes (value_, pending_increment_, flags_)
//   Relaxed        the same three, relaxed (ordering only)
//   TestFlag       two seq_cst RMWs, plus a relaxed load of flags_ (one RMW fewer)
//   RelaxedTestFlag two relaxed RMWs plus a relaxed flag load (the full prototype shape)
//   OneRmw         one relaxed RMW on value_; the pending delta would be derived at flush from a
//                  main-thread-only last value (a lower bound for any shared design)
//   Sharded        per-thread padded cells, summed on read (the read is not timed): no
//                  cache-line transfer at all, the prize of a sharded design
//
// Each runs on one shared object and on per-thread objects at least 256 bytes apart (different
// 128-byte prefetch pairs), with the timing, pinning and counters of bench_pinning.h.
// bmModelCurrentShared is the model check against stat_update_benchmark's bmCounterIncShared,
// at N = 1 and at the top N; a mismatch under contention points at layout (virtual call, the
// line shared with the name and refcount fields), not at the model.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "test/common/stats/bench_pinning.h"

#include "benchmark/benchmark.h"

namespace Envoy {
namespace Stats {
namespace {

constexpr uint16_t UsedFlag = 0x01;
constexpr size_t PrefetchPair = 128;
constexpr size_t MaxThreads = 64;

struct ModelCounter {
  std::atomic<uint32_t> ref_count_{1};
  std::atomic<uint16_t> flags_{0};
  std::atomic<uint64_t> value_{0};
  std::atomic<uint64_t> pending_increment_{0};
};

// One shard per thread, each in its own 128-byte prefetch pair.
struct alignas(PrefetchPair) ShardCell {
  std::atomic<uint64_t> value_{0};
};

struct ShardedCounter {
  ShardCell cells_[MaxThreads];
};

// Every model object starts its own 256-byte region.
struct alignas(2 * PrefetchPair) SpacedModel {
  ModelCounter counter;
  ShardedCounter sharded;
};

inline void incCurrent(ModelCounter& c) {
  c.value_ += 1;
  c.pending_increment_ += 1;
  c.flags_ |= UsedFlag;
}

inline void incRelaxed(ModelCounter& c) {
  c.value_.fetch_add(1, std::memory_order_relaxed);
  c.pending_increment_.fetch_add(1, std::memory_order_relaxed);
  c.flags_.fetch_or(UsedFlag, std::memory_order_relaxed);
}

inline void incTestFlag(ModelCounter& c) {
  c.value_ += 1;
  c.pending_increment_ += 1;
  if ((c.flags_.load(std::memory_order_relaxed) & UsedFlag) == 0) {
    c.flags_ |= UsedFlag;
  }
}

inline void incRelaxedTestFlag(ModelCounter& c) {
  c.value_.fetch_add(1, std::memory_order_relaxed);
  c.pending_increment_.fetch_add(1, std::memory_order_relaxed);
  if ((c.flags_.load(std::memory_order_relaxed) & UsedFlag) == 0) {
    c.flags_.fetch_or(UsedFlag, std::memory_order_relaxed);
  }
}

inline void incOneRmw(ModelCounter& c) { c.value_.fetch_add(1, std::memory_order_relaxed); }

std::vector<std::unique_ptr<SpacedModel>> models;
BenchPinning::RunSync run_sync;

enum class Layout { Shared, PerThreadSpaced };

template <Layout layout> void setup(const ::benchmark::State& state) {
  models.clear();
  const int count = layout == Layout::Shared ? 1 : state.threads();
  for (int i = 0; i < count; ++i) {
    models.push_back(std::make_unique<SpacedModel>());
  }
  run_sync.reset(state.threads());
}
void teardown(const ::benchmark::State&) { models.clear(); }

template <Layout layout> SpacedModel& model(const ::benchmark::State& state) {
  return *models[layout == Layout::Shared ? 0 : state.thread_index()];
}

template <Layout layout, void (*Inc)(ModelCounter&)> void modelBody(::benchmark::State& state) {
  ModelCounter& counter = model<layout>(state).counter;
  if (state.thread_index() == 0) {
    state.counters["model_bytes"] = sizeof(ModelCounter);
  }
  BenchPinning::timedLoop(state, run_sync, 0, [&counter]() { Inc(counter); });
}

template <Layout layout> void shardedBody(::benchmark::State& state) {
  if (static_cast<size_t>(state.threads()) > MaxThreads) {
    state.SkipWithError("more threads than shard cells");
    for (auto _ : state) { // NOLINT
    }
    return;
  }
  // With one shared sharded counter each thread has its own cell; with per-thread counters each
  // thread uses cell 0 of its own counter.
  ShardCell& cell =
      model<layout>(state).sharded.cells_[layout == Layout::Shared ? state.thread_index() : 0];
  BenchPinning::timedLoop(state, run_sync, 0,
                          [&cell]() { cell.value_.fetch_add(1, std::memory_order_relaxed); });
}

template <Layout layout> void registerModel(const char* name, void (*fn)(::benchmark::State&)) {
  ::benchmark::RegisterBenchmark(name, fn)
      ->Apply(BenchPinning::multiThreaded)
      ->Setup(setup<layout>)
      ->Teardown(teardown);
}

const bool registered = []() {
  registerModel<Layout::Shared>("bmModelCurrentShared", modelBody<Layout::Shared, incCurrent>);
  registerModel<Layout::PerThreadSpaced>("bmModelCurrentPerThreadSpaced",
                                         modelBody<Layout::PerThreadSpaced, incCurrent>);
  registerModel<Layout::Shared>("bmModelRelaxedShared", modelBody<Layout::Shared, incRelaxed>);
  registerModel<Layout::PerThreadSpaced>("bmModelRelaxedPerThreadSpaced",
                                         modelBody<Layout::PerThreadSpaced, incRelaxed>);
  registerModel<Layout::Shared>("bmModelTestFlagShared", modelBody<Layout::Shared, incTestFlag>);
  registerModel<Layout::PerThreadSpaced>("bmModelTestFlagPerThreadSpaced",
                                         modelBody<Layout::PerThreadSpaced, incTestFlag>);
  registerModel<Layout::Shared>("bmModelRelaxedTestFlagShared",
                                modelBody<Layout::Shared, incRelaxedTestFlag>);
  registerModel<Layout::PerThreadSpaced>("bmModelRelaxedTestFlagPerThreadSpaced",
                                         modelBody<Layout::PerThreadSpaced, incRelaxedTestFlag>);
  registerModel<Layout::Shared>("bmModelOneRmwShared", modelBody<Layout::Shared, incOneRmw>);
  registerModel<Layout::PerThreadSpaced>("bmModelOneRmwPerThreadSpaced",
                                         modelBody<Layout::PerThreadSpaced, incOneRmw>);
  registerModel<Layout::Shared>("bmModelShardedShared", shardedBody<Layout::Shared>);
  registerModel<Layout::PerThreadSpaced>("bmModelShardedPerThreadSpaced",
                                         shardedBody<Layout::PerThreadSpaced>);
  return true;
}();

} // namespace
} // namespace Stats
} // namespace Envoy
