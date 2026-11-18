// Note: this should be run with --compilation_mode=opt, on a quiescent system, with
// STATS_BENCH_CPUS set (see bench_pinning.h): the main dispatcher thread takes the first CPU and
// the N worker threads the next N.
//
// ParentHistogramImpl::recordValue() on real worker threads registered with a
// ThreadLocal::InstanceImpl, as in a running server: each worker records into its thread-local
// histogram, either for one histogram shared by all workers or for one histogram per worker,
// with or without the main thread merging histograms in a loop meanwhile. This shows whether the
// thread-local histogram path contends at all, and what the per-sample `used_` store costs.
//
// Each worker records its samples in chunks, re-posting itself between chunks, so that the merge
// loop's posts to the workers interleave with the recording. The record-only case (merge loop
// off) at N = 1 and at the top N is the smallest path that contains the `used_` store; each
// sample also pays the TLS cache lookup, the histogram insert and the sink check, and each chunk
// one dispatcher post.
//
// Worker counts come from STATS_BENCH_THREADS (bench_pinning.h). Per-worker parent histograms
// are created with spacer histograms in between and must be at least 256 bytes apart (checked).
// The thread-local histogram objects are allocated by each worker itself and are not checked.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "source/common/event/libevent.h"
#include "source/common/stats/thread_local_store.h"

#include "test/common/stats/bench_pinning.h"
#include "test/common/stats/real_thread_test_base.h"
#include "test/test_common/test_time.h"

#include "absl/strings/str_cat.h"
#include "absl/synchronization/blocking_counter.h"
#include "benchmark/benchmark.h"

namespace Envoy {
namespace Stats {
namespace {

constexpr uint64_t FullSamplesPerWorker = 100000;
// Under --skip_expensive_benchmarks (the *_benchmark_test pre-flight).
constexpr uint64_t PreflightSamplesPerWorker = 1000;

uint64_t samplesPerWorker() {
  return Envoy::benchmark::skipExpensiveBenchmarks() ? PreflightSamplesPerWorker
                                                     : FullSamplesPerWorker;
}
constexpr uint64_t SamplesPerChunk = 1000;
constexpr uint32_t SpacerHistograms = 4;

class HistogramRecordTest : public ThreadLocalRealThreadsMixin {
public:
  HistogramRecordTest(uint32_t num_workers, bool per_worker)
      : ThreadLocalRealThreadsMixin(num_workers), num_workers_(num_workers) {
    runOnMainBlocking([this, per_worker]() {
      pin_error_ = BenchPinning::pinOwnThread(0);
      const uint32_t count = per_worker ? num_workers_ : 1;
      for (uint32_t i = 0; i < count; ++i) {
        histograms_.push_back(
            &scope_.histogramFromString(absl::StrCat("h.", i), Histogram::Unit::Unspecified));
        for (uint32_t k = 0; k < SpacerHistograms; ++k) {
          scope_.histogramFromString(absl::StrCat("spacer.", i, ".", k),
                                     Histogram::Unit::Unspecified);
        }
      }
    });
  }

  // Smallest address distance between two workers' parent histograms.
  uintptr_t minDistance() const {
    uintptr_t min = UINTPTR_MAX;
    for (size_t a = 0; a < histograms_.size(); ++a) {
      for (size_t b = a + 1; b < histograms_.size(); ++b) {
        min = std::min(min, BenchPinning::distance(histograms_[a], histograms_[b]));
      }
    }
    return min;
  }

  const std::string& pinError() const { return pin_error_; }

  // Records samplesPerWorker() samples on every worker and returns the elapsed seconds. With
  // `merge_loop`, the main thread merges histograms back to back for the whole time.
  double recordOnce(bool merge_loop) {
    stop_merging_ = false;
    if (merge_loop) {
      merging_ = true;
      main_dispatcher_->post([this]() { mergeStep(); });
    }
    absl::BlockingCounter done(num_workers_);
    // Each worker's user-space cycles and instructions over its recording loops, opened on the
    // worker itself at its first chunk.
    perf_.clear();
    perf_.resize(num_workers_);
    worker_cycles_.assign(num_workers_, 0);
    worker_instructions_.assign(num_workers_, 0);
    const MonotonicTime start = time_.monotonicTime();
    for (uint32_t w = 0; w < num_workers_; ++w) {
      Histogram* histogram = histograms_[histograms_.size() == 1 ? 0 : w];
      Event::Dispatcher* dispatcher = thread_dispatchers_[w].get();
      auto chunk = std::make_shared<std::function<void()>>();
      const uint64_t samples = samplesPerWorker();
      auto remaining = std::make_shared<uint64_t>(samples);
      *chunk = [this, w, histogram, dispatcher, &done, chunk, remaining, samples]() {
        if (*remaining == samples) {
          if (!BenchPinning::pinOwnThread(w + 1).empty()) {
            pin_failed_ = true;
          }
          perf_[w] = std::make_unique<BenchPinning::ThreadPerfCounters>();
        }
        const uint64_t n = std::min(SamplesPerChunk, *remaining);
        perf_[w]->start();
        for (uint64_t i = 0; i < n; ++i) {
          histogram->recordValue((*remaining - i) * 7919 % 65536);
        }
        perf_[w]->stop();
        accumulate(worker_cycles_[w], perf_[w]->cycles());
        accumulate(worker_instructions_[w], perf_[w]->instructions());
        *remaining -= n;
        if (*remaining > 0) {
          dispatcher->post([chunk]() { (*chunk)(); });
        } else {
          // The closure holds `chunk`, a reference to itself; drop it from a separate task.
          dispatcher->post([chunk]() { *chunk = nullptr; });
          done.DecrementCount();
        }
      };
      dispatcher->post([chunk]() { (*chunk)(); });
    }
    done.Wait();
    const MonotonicTime end = time_.monotonicTime();
    stop_merging_ = true;
    while (merging_.load()) {
    }
    // Let the workers run the tasks that release the chunk closures.
    tlsBlock();
    return std::chrono::duration<double>(end - start).count();
  }

  bool pinFailed() const { return pin_failed_.load(); }
  uint64_t merges() const { return merges_.load(); }

  // Totals over the workers of the last recordOnce(), or -1 when a PMU counter was unavailable.
  double totalCycles() const { return total(worker_cycles_); }
  double totalInstructions() const { return total(worker_instructions_); }

private:
  static void accumulate(double& sum, double value) {
    sum = (sum < 0 || value < 0) ? -1 : sum + value;
  }
  static double total(const std::vector<double>& values) {
    double sum = 0;
    for (double v : values) {
      if (v < 0) {
        return -1;
      }
      sum += v;
    }
    return sum;
  }

  // Runs on the main thread: one merge, then the next, until told to stop.
  void mergeStep() {
    if (stop_merging_.load()) {
      merging_ = false;
      return;
    }
    merges_.fetch_add(1);
    store_->mergeHistograms([this]() { main_dispatcher_->post([this]() { mergeStep(); }); });
  }

  const uint32_t num_workers_;
  std::vector<Histogram*> histograms_;
  std::string pin_error_;
  std::atomic<bool> pin_failed_{false};
  std::atomic<bool> stop_merging_{false};
  std::atomic<bool> merging_{false};
  std::atomic<uint64_t> merges_{0};
  // Written by worker w only, read by the driver after the workers are done.
  std::vector<std::unique_ptr<BenchPinning::ThreadPerfCounters>> perf_;
  std::vector<double> worker_cycles_;
  std::vector<double> worker_instructions_;
  Event::TestRealTimeSystem time_;
};

void runHistogramRecord(::benchmark::State& state, uint32_t workers, bool per_worker,
                        bool merge_loop) {
  // test/benchmark/main.cc does not initialize libevent, and the real-threads fixture creates
  // dispatchers; initialize it as thread_local_store_speed_test.cc does.
  if (!Envoy::Event::Libevent::Global::initialized()) {
    Envoy::Event::Libevent::Global::initialize();
  }
  if (Envoy::benchmark::skipExpensiveBenchmarks() && workers > 2) {
    state.SkipWithError("Skipping expensive benchmark");
    return;
  }
  HistogramRecordTest test(workers, per_worker);
  if (!test.pinError().empty()) {
    state.SkipWithError(test.pinError().c_str());
    return;
  }
  if (per_worker && workers > 1) {
    const uintptr_t min = test.minDistance();
    state.counters["min_distance_bytes"] = static_cast<double>(min);
    if (min < BenchPinning::MinSpacedDistance) {
      state.SkipWithError("per-worker histograms are closer than 256 bytes");
      return;
    }
  }
  state.SetLabel(absl::StrCat(BenchPinning::label(), merge_loop ? " merge" : " record-only"));
  double seconds = 0;
  double cycles = 0;
  double instructions = 0;
  for (auto _ : state) { // NOLINT
    const double iteration = test.recordOnce(merge_loop);
    seconds += iteration;
    state.SetIterationTime(iteration);
    const double c = test.totalCycles();
    const double i = test.totalInstructions();
    cycles = (cycles < 0 || c < 0) ? -1 : cycles + c;
    instructions = (instructions < 0 || i < 0) ? -1 : instructions + i;
  }
  if (test.pinFailed()) {
    state.SkipWithError("could not pin every worker thread");
    return;
  }
  const double samples = static_cast<double>(state.iterations()) * samplesPerWorker();
  state.SetItemsProcessed(state.iterations() * workers * samplesPerWorker());
  // Workers record in parallel, so the time per sample on one worker is the elapsed time over
  // the samples each worker recorded.
  state.counters["ns_per_sample"] = samples > 0 ? seconds * 1e9 / samples : 0;
  // Cycles and instructions of the recording loops only, summed over workers and divided by all
  // samples recorded: the per-sample cost the per-request arithmetic needs.
  const double all_samples = samples * workers;
  state.counters["cycles_per_sample"] =
      (cycles < 0 || all_samples == 0) ? -1 : cycles / all_samples;
  state.counters["instructions_per_sample"] =
      (instructions < 0 || all_samples == 0) ? -1 : instructions / all_samples;
  state.counters["clocksource_ok"] = BenchPinning::clockSourceOk();
  state.counters["merges"] = test.merges();
}

// Arguments: workers N, one histogram per worker (1) or shared (0), merge loop (1) or not (0).
void bmHistogramRecord(::benchmark::State& state) {
  runHistogramRecord(state, state.range(0), state.range(1) != 0, state.range(2) != 0);
}

// The record-only measurement of the per-sample used_ store: one histogram per worker, no merge
// loop. Argument: workers N.
void bmHistogramRecordOnly(::benchmark::State& state) {
  runHistogramRecord(state, state.range(0), /*per_worker=*/true, /*merge_loop=*/false);
}

std::vector<int64_t> workerCounts() {
  std::vector<int64_t> workers;
  for (int n : BenchPinning::threadCounts()) {
    workers.push_back(n);
  }
  return workers;
}

void histogramArgs(::benchmark::internal::Benchmark* b) {
  b->ArgsProduct({workerCounts(), {0, 1}, {0, 1}});
}

void recordOnlyArgs(::benchmark::internal::Benchmark* b) { b->ArgsProduct({workerCounts()}); }

BENCHMARK(bmHistogramRecordOnly)
    ->Unit(::benchmark::kMillisecond)
    ->UseManualTime()
    ->MinTime(BenchPinning::MinTimeSeconds)
    ->Apply(recordOnlyArgs);
BENCHMARK(bmHistogramRecord)
    ->Unit(::benchmark::kMillisecond)
    ->UseManualTime()
    ->MinTime(BenchPinning::MinTimeSeconds)
    ->Apply(histogramArgs);

} // namespace
} // namespace Stats
} // namespace Envoy
