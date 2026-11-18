#pragma once

// Shared mechanics for the multi-threaded stats benchmarks.
//
// Placement. Google Benchmark does not pin its threads, so each benchmark thread pins itself to
// one CPU from STATS_BENCH_CPUS, a list such as "96-111" or "0-13,20-33", by thread_index(), and
// restores its previous affinity when the benchmark body ends (the calling thread is one of the
// benchmark threads, and later benchmarks must not inherit a one-CPU mask). STATS_BENCH_PLACEMENT
// is a free-form label ("one-socket", "cross-socket", "smt-pair"; the SMT pair is never a
// headline) copied into the benchmark label. List CPUs one per physical core, siblings idle.
//
// Thread counts. STATS_BENCH_THREADS lists the thread (or worker) counts to register, for example
// "1,2,4,8,12,15" on a 16-core VM; STATS_BENCH_HAMMERS the hammer counts of the flush benchmark.
// A count larger than the CPU list skips itself.
//
// Timing. The multi-threaded benchmarks run one Google Benchmark iteration with manual time;
// inside it every thread runs the operation for a time-bounded window between shared deadlines,
// and keeps running until every thread has finished its window, so no window has a tail with
// fewer contenders (see timedLoop()). Per-thread user-space cycles and instructions come from
// perf_event_open. Fairness and per-thread operations are results; window overlap, involuntary
// context switches, steal and the operations floor are reported for the runner to gate on.

#include <time.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "test/benchmark/main.h"

#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"
#include "benchmark/benchmark.h"

#ifdef __linux__
#include <linux/perf_event.h>
#include <sys/resource.h>
#include <pthread.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Envoy {
namespace Stats {
namespace BenchPinning {

// Minimum distance between the start addresses of two threads' objects. The x86 spatial
// prefetcher pairs 64-byte lines into 128-byte blocks and a stat object can straddle two lines,
// so neighbours must fall in different 128-byte pairs; 256 bytes guarantees that for objects up
// to 128 bytes.
constexpr uintptr_t MinSpacedDistance = 256;
constexpr double MinTimeSeconds = 1.0;
constexpr double WarmUpSeconds = 0.2;

inline uintptr_t distance(const void* a, const void* b) {
  const uintptr_t x = reinterpret_cast<uintptr_t>(a);
  const uintptr_t y = reinterpret_cast<uintptr_t>(b);
  return x > y ? x - y : y - x;
}

// Parses "1,2,4" or "96-99,100" into integers; empty on a malformed list.
inline std::vector<int> parseList(const char* text) {
  std::vector<int> result;
  if (text == nullptr) {
    return result;
  }
  for (absl::string_view part : absl::StrSplit(text, ',', absl::SkipEmpty())) {
    std::vector<absl::string_view> range = absl::StrSplit(part, '-');
    int first = 0;
    int last = 0;
    if (range.empty() || range.size() > 2 || !absl::SimpleAtoi(range[0], &first)) {
      return {};
    }
    last = first;
    if (range.size() == 2 && !absl::SimpleAtoi(range[1], &last)) {
      return {};
    }
    for (int value = first; value <= last; ++value) {
      result.push_back(value);
    }
  }
  return result;
}

inline std::vector<int> cpus() { return parseList(std::getenv("STATS_BENCH_CPUS")); }

inline std::vector<int> listOrDefault(const char* env_name, std::vector<int> default_list) {
  std::vector<int> list = parseList(std::getenv(env_name));
  return list.empty() ? default_list : list;
}

// Thread (or worker) counts: one per physical core, so the default spans the measured rungs
// (x86 VM and Arm VM tops 12 and 15, the x86 server one socket 14 and both sockets 28).
inline std::vector<int> threadCounts() {
  return listOrDefault("STATS_BENCH_THREADS", {1, 2, 4, 8, 12, 14, 15, 28});
}

// Hammer counts for the flush benchmark; the flushing thread takes one more CPU.
inline std::vector<int> hammerCounts() {
  return listOrDefault("STATS_BENCH_HAMMERS", {0, 1, 4, 8, 14});
}

// The kernel's current clock source. The window design reads the clock once per batch and
// assumes a vDSO clock source; on one that falls back to a system call the read costs about as
// much as an uncontended batch.
inline std::string clockSource() {
  std::string source = "unknown";
#ifdef __linux__
  FILE* file = std::fopen("/sys/devices/system/clocksource/clocksource0/current_clocksource", "r");
  if (file != nullptr) {
    char buffer[64] = {};
    if (std::fgets(buffer, sizeof(buffer), file) != nullptr) {
      source = std::string(absl::StripTrailingAsciiWhitespace(buffer));
    }
    std::fclose(file);
  }
#endif
  return source;
}

// 1 if the clock source is one with a vDSO read (tsc, kvm-clock, arch_sys_counter), else 0.
inline double clockSourceOk() {
  const std::string source = clockSource();
  return (source == "tsc" || source == "kvm-clock" || source == "arch_sys_counter") ? 1 : 0;
}

inline std::string label() {
  const char* cpus_env = std::getenv("STATS_BENCH_CPUS");
  const char* placement = std::getenv("STATS_BENCH_PLACEMENT");
  return absl::StrCat("cpus=", cpus_env != nullptr ? cpus_env : "unpinned",
                      " placement=", placement != nullptr ? placement : "unset",
                      " clocksource=", clockSource());
}

inline double monotonicSeconds() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

// Pins the calling thread to the index'th CPU of STATS_BENCH_CPUS for its lifetime and restores
// the previous affinity on destruction. With STATS_BENCH_CPUS unset it does nothing.
class ScopedPin {
public:
  explicit ScopedPin(size_t index) {
    if (std::getenv("STATS_BENCH_CPUS") == nullptr) {
      return;
    }
    const std::vector<int> list = cpus();
    if (index >= list.size()) {
      error_ = absl::StrCat("STATS_BENCH_CPUS lists ", list.size(), " CPUs, thread ", index,
                            " needs more");
      return;
    }
#ifdef __linux__
    if (pthread_getaffinity_np(pthread_self(), sizeof(saved_), &saved_) != 0) {
      error_ = "pthread_getaffinity_np failed";
      return;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(list[index], &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
      error_ = absl::StrCat("pthread_setaffinity_np failed for CPU ", list[index]);
      return;
    }
    restore_ = true;
#else
    error_ = "thread pinning is only implemented on Linux";
#endif
  }

  ~ScopedPin() {
#ifdef __linux__
    if (restore_) {
      pthread_setaffinity_np(pthread_self(), sizeof(saved_), &saved_);
    }
#endif
  }

  ScopedPin(const ScopedPin&) = delete;
  ScopedPin& operator=(const ScopedPin&) = delete;

  const std::string& error() const { return error_; }

private:
  std::string error_;
  bool restore_ = false;
#ifdef __linux__
  cpu_set_t saved_;
#endif
};

// Pins the calling thread for the rest of its life. For threads a benchmark creates itself (worker,
// main dispatcher and hammer threads), which exit when the benchmark ends, so nothing is restored.
// Returns an empty string on success or when STATS_BENCH_CPUS is unset.
inline std::string pinOwnThread(size_t index) {
  if (std::getenv("STATS_BENCH_CPUS") == nullptr) {
    return "";
  }
  const std::vector<int> list = cpus();
  if (index >= list.size()) {
    return absl::StrCat("STATS_BENCH_CPUS lists ", list.size(), " CPUs, thread ", index,
                        " needs more");
  }
#ifdef __linux__
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(list[index], &set);
  if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
    return absl::StrCat("pthread_setaffinity_np failed for CPU ", list[index]);
  }
  return "";
#else
  return "thread pinning is only implemented on Linux";
#endif
}

// User-space cycles and instructions of the calling thread, read with perf_event_open. When the
// counters cannot be opened (for example perf_event_paranoid, or no PMU in a VM) the values are
// negative and are reported as such.
class ThreadPerfCounters {
public:
  ThreadPerfCounters() {
#ifdef __linux__
    cycles_fd_ = open(PERF_COUNT_HW_CPU_CYCLES);
    instructions_fd_ = open(PERF_COUNT_HW_INSTRUCTIONS);
#endif
  }

  ~ThreadPerfCounters() {
#ifdef __linux__
    if (cycles_fd_ >= 0) {
      close(cycles_fd_);
    }
    if (instructions_fd_ >= 0) {
      close(instructions_fd_);
    }
#endif
  }

  ThreadPerfCounters(const ThreadPerfCounters&) = delete;
  ThreadPerfCounters& operator=(const ThreadPerfCounters&) = delete;

  void start() {
    cycles_start_ = read(cycles_fd_);
    instructions_start_ = read(instructions_fd_);
  }
  void stop() {
    cycles_ = delta(cycles_fd_, cycles_start_);
    instructions_ = delta(instructions_fd_, instructions_start_);
  }
  double cycles() const { return cycles_; }
  double instructions() const { return instructions_; }

private:
#ifdef __linux__
  static int open(uint64_t config) {
    struct perf_event_attr attr = {};
    attr.type = PERF_TYPE_HARDWARE;
    attr.size = sizeof(attr);
    attr.config = config;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    return static_cast<int>(syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0));
  }
#endif
  static int64_t read(int fd) {
#ifdef __linux__
    uint64_t value = 0;
    if (fd >= 0 && ::read(fd, &value, sizeof(value)) == sizeof(value)) {
      return static_cast<int64_t>(value);
    }
#else
    (void)fd;
#endif
    return -1;
  }
  static double delta(int fd, int64_t start) {
    const int64_t end = read(fd);
    return (start < 0 || end < 0) ? -1.0 : static_cast<double>(end - start);
  }

  int cycles_fd_ = -1;
  int instructions_fd_ = -1;
  int64_t cycles_start_ = -1;
  int64_t instructions_start_ = -1;
  double cycles_ = -1;
  double instructions_ = -1;
};

// Window lengths of the time-bounded measurement: STATS_BENCH_WARMUP and STATS_BENCH_SECONDS, in
// seconds (defaults 0.2 and 1.0). Under --skip_expensive_benchmarks (the *_benchmark_test
// pre-flight) they shrink to 0.01 and 0.05 unless set explicitly.
inline double envSeconds(const char* name, double default_seconds, double preflight_seconds) {
  const char* env = std::getenv(name);
  double value = 0;
  if (env != nullptr && absl::SimpleAtod(env, &value) && value > 0) {
    return value;
  }
  return Envoy::benchmark::skipExpensiveBenchmarks() ? preflight_seconds : default_seconds;
}

// Operations between two clock reads in the timed window. The clock read costs the same in every
// variant (about 20 ns per batch).
constexpr uint64_t OpsPerBatch = 256;

// What one thread measured over its window.
struct ThreadWindow {
  uint64_t ops = 0;
  double start = 0;
  double end = 0;
  double cycles = -1;
  double instructions = -1;
  int64_t involuntary_switches = -1;
};

inline int64_t involuntarySwitches() {
#if defined(__linux__) && defined(RUSAGE_THREAD)
  struct rusage usage;
  if (getrusage(RUSAGE_THREAD, &usage) == 0) {
    return usage.ru_nivcsw;
  }
#endif
  return -1;
}

// Total steal ticks of the machine from /proc/stat (field 8 of the "cpu" line), or -1.
inline int64_t stealTicks() {
#ifdef __linux__
  FILE* file = std::fopen("/proc/stat", "r");
  if (file == nullptr) {
    return -1;
  }
  unsigned long long v[8] = {};
  const int n = std::fscanf(file, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1],
                            &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
  std::fclose(file);
  return n == 8 ? static_cast<int64_t>(v[7]) : -1;
#else
  return -1;
#endif
}

// Per-run state shared by the threads of one benchmark run. reset() is called from the
// benchmark's Setup, before the threads start.
class RunSync {
public:
  void reset(int threads) {
    threads_ = threads;
    ready_.store(0);
    done_.store(0);
    go_.store(false);
    windows_.assign(threads, ThreadWindow());
  }
  int threads() const { return threads_; }

  // Start barrier inside the timed region, after pinning: the last thread to arrive is not
  // special; thread 0 waits for all, sets the shared deadlines and releases everyone.
  void startBarrier(int thread) {
    ready_.fetch_add(1);
    if (thread == 0) {
      while (ready_.load() < threads_) {
      }
      const double now = monotonicSeconds();
      warmup_end_ = now + envSeconds("STATS_BENCH_WARMUP", 0.2, 0.01);
      measure_end_ = warmup_end_ + envSeconds("STATS_BENCH_SECONDS", 1.0, 0.05);
      steal_start_ = stealTicks();
      go_.store(true, std::memory_order_release);
    } else {
      while (!go_.load(std::memory_order_acquire)) {
      }
    }
  }
  double warmupEnd() const { return warmup_end_; }
  double measureEnd() const { return measure_end_; }

  void finished() { done_.fetch_add(1); }
  bool allFinished() const { return done_.load() >= threads_; }

  ThreadWindow& window(int thread) { return windows_[thread]; }

  // Valid on thread 0 after the benchmark loop (Google Benchmark's stop barrier orders every
  // thread's window before it).
  void report(::benchmark::State& state) {
    double earliest_start = windows_[0].start;
    double latest_start = windows_[0].start;
    double earliest_end = windows_[0].end;
    double latest_end = windows_[0].end;
    uint64_t min_ops = windows_[0].ops;
    uint64_t max_ops = windows_[0].ops;
    double ns_per_op = 0;
    double ops_per_sec = 0;
    double cycles = 0;
    double instructions = 0;
    double total_cycles = 0;
    double total_instructions = 0;
    double total_ops = 0;
    bool have_pmu = true;
    int64_t switches = 0;
    bool have_switches = true;
    for (const ThreadWindow& w : windows_) {
      earliest_start = std::min(earliest_start, w.start);
      latest_start = std::max(latest_start, w.start);
      earliest_end = std::min(earliest_end, w.end);
      latest_end = std::max(latest_end, w.end);
      min_ops = std::min(min_ops, w.ops);
      max_ops = std::max(max_ops, w.ops);
      const double seconds = w.end - w.start;
      if (w.ops > 0) {
        ns_per_op += seconds * 1e9 / w.ops / threads_;
        ops_per_sec += w.ops / seconds;
        total_ops += w.ops;
        if (w.cycles < 0 || w.instructions < 0) {
          have_pmu = false;
        } else {
          cycles += w.cycles / w.ops / threads_;
          instructions += w.instructions / w.ops / threads_;
          total_cycles += w.cycles;
          total_instructions += w.instructions;
        }
      }
      if (w.involuntary_switches < 0) {
        have_switches = false;
      } else {
        switches += w.involuntary_switches;
      }
    }
    const double span = latest_end - earliest_start;
    state.counters["ns_per_op"] = ns_per_op;
    state.counters["ops_per_sec"] = ops_per_sec;
    // Totals over all threads: what the per-request arithmetic needs. They differ from the
    // per-thread means whenever fairness is below 1, which is the contended case.
    state.counters["cycles_per_op"] = (have_pmu && total_ops > 0) ? total_cycles / total_ops : -1;
    state.counters["instructions_per_op"] =
        (have_pmu && total_ops > 0) ? total_instructions / total_ops : -1;
    state.counters["cycles_per_op_thread_mean"] = have_pmu ? cycles : -1;
    state.counters["instructions_per_op_thread_mean"] = have_pmu ? instructions : -1;
    state.counters["clocksource_ok"] = clockSourceOk();
    state.counters["fairness"] = max_ops > 0 ? static_cast<double>(min_ops) / max_ops : 0;
    state.counters["min_thread_ops"] = static_cast<double>(min_ops);
    state.counters["window_overlap"] = span > 0 ? (earliest_end - latest_start) / span : 0;
    state.counters["invol_switches"] = have_switches ? static_cast<double>(switches) : -1;
    const int64_t steal_end = stealTicks();
    state.counters["steal_ticks"] =
        (steal_start_ < 0 || steal_end < 0) ? -1 : static_cast<double>(steal_end - steal_start_);
  }

private:
  int threads_ = 0;
  std::atomic<int> ready_{0};
  std::atomic<int> done_{0};
  std::atomic<bool> go_{false};
  double warmup_end_ = 0;
  double measure_end_ = 0;
  int64_t steal_start_ = -1;
  std::vector<ThreadWindow> windows_;
};

// Runs `op` for a time-bounded window inside the single Google Benchmark iteration (registered
// with multiThreaded()):
//   * each thread pins itself by thread_index(), restores its affinity at the end, and opens its
//     perf counters, then waits at a start barrier inside the timed region;
//   * the shared deadlines put a warm-up before the window, and every thread snapshots its
//     operations, time, cycles, instructions and involuntary context switches at the first
//     batch boundary past each deadline;
//   * after its window a thread keeps running `op` until every thread has finished its window,
//     so each window is covered by all other threads at full load: there is no tail with fewer
//     contenders, however unevenly a contended line is served.
// Thread 0 reports ns_per_op (what the runner reads; not the time column), ops_per_sec,
// cycles_per_op and instructions_per_op (totals over all threads / total operations; the
// per-thread means are *_thread_mean), fairness and min_thread_ops as results, and
// window_overlap, invol_switches, steal_ticks and clocksource_ok as gate inputs; the runner gates,
// the benchmark never aborts on them. The clock read and loop overhead are the same in every arm,
// so relaxed/current ratios read slightly closer to 1 than the truth. Returns false if the run
// was skipped.
template <class Op>
bool timedLoop(::benchmark::State& state, RunSync& sync, size_t first_cpu_index, Op op) {
  // Uniform checks first, so that either every thread skips or none does.
  const std::vector<int> cpu_list = cpus();
  if (std::getenv("STATS_BENCH_CPUS") != nullptr &&
      cpu_list.size() < static_cast<size_t>(state.threads()) + first_cpu_index) {
    state.SkipWithError(absl::StrCat("STATS_BENCH_CPUS lists ", cpu_list.size(), " CPUs, ",
                                     state.threads() + first_cpu_index, " needed")
                            .c_str());
    return false;
  }
  if (Envoy::benchmark::skipExpensiveBenchmarks() && state.threads() > 4) {
    state.SkipWithError("Skipping expensive benchmark");
    return false;
  }

  const int thread = state.thread_index();
  ScopedPin pin(thread + first_cpu_index);
  const bool pinned = pin.error().empty();
  if (thread == 0) {
    state.SetLabel(label());
  }
  ThreadWindow& window = sync.window(thread);
  for (auto _ : state) { // NOLINT: exactly one iteration (Iterations(1)).
    ThreadPerfCounters perf;
    sync.startBarrier(thread);
    const double warmup_end = sync.warmupEnd();
    const double measure_end = sync.measureEnd();
    int phase = 0;
    uint64_t ops = 0;
    uint64_t ops_at_start = 0;
    int64_t switches_at_start = 0;
    while (true) {
      if (pinned) {
        for (uint64_t i = 0; i < OpsPerBatch; ++i) {
          op();
        }
        ops += OpsPerBatch;
      }
      const double now = monotonicSeconds();
      if (phase == 0 && now >= warmup_end) {
        ops_at_start = ops;
        window.start = now;
        switches_at_start = involuntarySwitches();
        perf.start();
        phase = 1;
      } else if (phase == 1 && now >= measure_end) {
        perf.stop();
        window.end = now;
        window.ops = ops - ops_at_start;
        window.cycles = perf.cycles();
        window.instructions = perf.instructions();
        const int64_t switches = involuntarySwitches();
        window.involuntary_switches =
            (switches < 0 || switches_at_start < 0) ? -1 : switches - switches_at_start;
        sync.finished();
        phase = 2;
      }
      if (phase == 2 && sync.allFinished()) {
        break;
      }
    }
    state.SetIterationTime(window.end - window.start);
  }
  state.SetItemsProcessed(window.ops);
  if (!pinned) {
    state.SkipWithError(pin.error().c_str());
    return false;
  }
  if (thread == 0) {
    sync.report(state);
  }
  return true;
}

// Registers the thread counts above with the time-bounded window: one Google Benchmark
// iteration per run and manual time, so there are no iteration-estimation runs.
inline void multiThreaded(::benchmark::internal::Benchmark* b) {
  for (int threads : threadCounts()) {
    b->Threads(threads);
  }
  b->Iterations(1)->UseManualTime();
}

} // namespace BenchPinning
} // namespace Stats
} // namespace Envoy
