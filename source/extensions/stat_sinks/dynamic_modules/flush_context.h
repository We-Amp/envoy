#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "envoy/common/time.h"
#include "envoy/event/dispatcher.h"
#include "envoy/stats/histogram.h"
#include "envoy/stats/primitive_stats.h"
#include "envoy/stats/sink.h"
#include "envoy/stats/stats.h"

#include "source/common/stats/symbol_table.h"

namespace Envoy {
namespace Extensions {
namespace StatSinks {
namespace DynamicModules {

/**
 * Values copied when a module retains a snapshot, indexed like the snapshot's collections. The
 * snapshot callbacks read these instead of the live stats for a retained snapshot, so the module
 * sees one point in time and never races the main thread's histogram merge.
 */
struct DynamicModuleStatsSinkFrozenValues {
  std::vector<uint64_t> counter_values_;
  std::vector<uint64_t> gauge_values_;
  std::vector<std::string> text_readout_values_;
  std::vector<uint64_t> histogram_sample_counts_;
  std::vector<double> histogram_sample_sums_;
  // Histogram h's buckets are [histogram_bucket_offsets_[h], histogram_bucket_offsets_[h + 1]) in
  // the two flat arrays below, so a flush allocates three bucket vectors in total rather than two
  // per histogram.
  std::vector<size_t> histogram_bucket_offsets_;
  std::vector<double> histogram_bucket_bounds_;
  std::vector<uint64_t> histogram_bucket_counts_;
};

class DynamicModuleStatsSinkRetainedSnapshot;

/**
 * Snapshot handle passed to the module as the opaque snapshot pointer.
 *
 * The flush hook gets a handle that borrows the flush snapshot and is valid only during that hook.
 * envoy_dynamic_module_callback_stat_sink_snapshot_retain returns a handle owned by a
 * DynamicModuleStatsSinkRetainedSnapshot, which serves as its snapshot and supplies the values
 * copied at retain time. The snapshot callbacks decode stat names directly into module-provided
 * buffers. A small tag cache `memoizes` the last metric's tags so a full per-metric tag read stays
 * linear.
 */
struct DynamicModuleStatsSinkFlushContext {
  DynamicModuleStatsSinkFlushContext(Stats::MetricSnapshot& snapshot,
                                     Event::Dispatcher& main_thread_dispatcher)
      : snapshot_(snapshot), main_thread_dispatcher_(main_thread_dispatcher) {}
  DynamicModuleStatsSinkFlushContext(Stats::MetricSnapshot& snapshot,
                                     Event::Dispatcher& main_thread_dispatcher,
                                     const DynamicModuleStatsSinkFrozenValues& frozen,
                                     DynamicModuleStatsSinkRetainedSnapshot& owner)
      : snapshot_(snapshot), main_thread_dispatcher_(main_thread_dispatcher), frozen_(&frozen),
        owner_(&owner) {}

  Stats::MetricSnapshot& snapshot_;
  // Retained snapshots released off the main thread are deleted here; see
  // envoy_dynamic_module_callback_stat_sink_snapshot_release.
  Event::Dispatcher& main_thread_dispatcher_;
  // Set only for a retained snapshot.
  const DynamicModuleStatsSinkFrozenValues* frozen_ = nullptr;
  DynamicModuleStatsSinkRetainedSnapshot* owner_ = nullptr;

  // Caches the decoded tag StatNames of the most recently read metric. A module reads a metric's
  // tags in order, so `memoizing` the last metric collapses the per-index tag walks from O(T^2) to
  // O(T) for a full read. The StatNames borrow the metric, which the snapshot keeps alive.
  const Stats::Metric* cached_metric_ = nullptr;
  std::vector<Stats::StatNameTag> cached_tags_;
};

/**
 * A snapshot a module kept past its flush hook. It holds a reference on every stat, the same way
 * Envoy's own per-flush snapshot does, so stat names and tags stay readable on any thread after the
 * flush returns. Values are copied at retain time into DynamicModuleStatsSinkFrozenValues.
 *
 * It is created by envoy_dynamic_module_callback_stat_sink_snapshot_retain, on the main thread from
 * the flush hook's handle or on any thread from another retained handle, and deleted by
 * envoy_dynamic_module_callback_stat_sink_snapshot_release, which the module may call on any
 * thread. Reads after construction only touch the frozen values and the immutable name and tag
 * storage of the referenced stats, so one thread at a time may read it anywhere.
 *
 * The module may release it on any thread. A release off the main thread posts the deletion to the
 * main thread dispatcher, so the stat references it holds are always dropped on the main thread.
 * Stats are only destroyed on the main thread, which is what lets the per-flush snapshot read stats
 * without holding a reference to each of them.
 */
class DynamicModuleStatsSinkRetainedSnapshot : public Stats::MetricSnapshot {
public:
  // `source` is either the flush hook's handle, in which case this must run on the main thread
  // during that hook, or another retained snapshot's handle, in which case it may run on whichever
  // thread holds that handle.
  explicit DynamicModuleStatsSinkRetainedSnapshot(DynamicModuleStatsSinkFlushContext& source);
  // context_ points back into this object, so it must never be copied.
  DynamicModuleStatsSinkRetainedSnapshot(const DynamicModuleStatsSinkRetainedSnapshot&) = delete;
  DynamicModuleStatsSinkRetainedSnapshot&
  operator=(const DynamicModuleStatsSinkRetainedSnapshot&) = delete;

  // Stats::MetricSnapshot
  const std::vector<CounterSnapshot>& counters() override { return counters_; }
  const std::vector<std::reference_wrapper<const Stats::Gauge>>& gauges() override {
    return gauges_;
  }
  const std::vector<std::reference_wrapper<const Stats::ParentHistogram>>& histograms() override {
    return histograms_;
  }
  const std::vector<std::reference_wrapper<const Stats::TextReadout>>& textReadouts() override {
    return text_readouts_;
  }
  // Host stats are not exposed through the module ABI, so a retained snapshot keeps none.
  const std::vector<Stats::PrimitiveCounterSnapshot>& hostCounters() override {
    return host_counters_;
  }
  const std::vector<Stats::PrimitiveGaugeSnapshot>& hostGauges() override { return host_gauges_; }
  SystemTime snapshotTime() const override { return snapshot_time_; }

  DynamicModuleStatsSinkFlushContext& context() { return context_; }

private:
  std::vector<Stats::CounterSharedPtr> snapped_counters_;
  std::vector<CounterSnapshot> counters_;
  std::vector<Stats::GaugeSharedPtr> snapped_gauges_;
  std::vector<std::reference_wrapper<const Stats::Gauge>> gauges_;
  std::vector<Stats::ParentHistogramSharedPtr> snapped_histograms_;
  std::vector<std::reference_wrapper<const Stats::ParentHistogram>> histograms_;
  std::vector<Stats::TextReadoutSharedPtr> snapped_text_readouts_;
  std::vector<std::reference_wrapper<const Stats::TextReadout>> text_readouts_;
  const std::vector<Stats::PrimitiveCounterSnapshot> host_counters_;
  const std::vector<Stats::PrimitiveGaugeSnapshot> host_gauges_;
  const SystemTime snapshot_time_;
  DynamicModuleStatsSinkFrozenValues frozen_;
  DynamicModuleStatsSinkFlushContext context_;
};

} // namespace DynamicModules
} // namespace StatSinks
} // namespace Extensions
} // namespace Envoy
