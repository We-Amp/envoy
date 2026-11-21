#pragma once

#include <optional>

#include "envoy/buffer/buffer.h"
#include "envoy/common/platform.h"
#include "envoy/event/dispatcher.h"
#include "envoy/event/schedulable_cb.h"
#include "envoy/local_info/local_info.h"
#include "envoy/network/connection.h"
#include "envoy/stats/histogram.h"
#include "envoy/stats/scope.h"
#include "envoy/stats/sink.h"
#include "envoy/stats/stats.h"
#include "envoy/stats/tag.h"
#include "envoy/thread_local/thread_local.h"
#include "envoy/upstream/cluster_manager.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/common/macros.h"
#include "source/common/network/io_socket_handle_impl.h"
#include "source/extensions/stat_sinks/common/statsd/tag_formats.h"

namespace Envoy {
namespace Extensions {
namespace StatSinks {
namespace Common {
namespace Statsd {

static const std::string& getDefaultPrefix() { CONSTRUCT_ON_FIRST_USE(std::string, "envoy"); }

/**
 * Scales a histogram sample to the milliseconds reported for a statsd timer when the histogram's
 * unit requires it. Samples of histograms recording nanoseconds or microseconds are scaled. Samples
 * of histograms recording milliseconds are reported unchanged, as are those of histograms without a
 * unit (many of which measure milliseconds already) and of byte histograms, for which statsd has no
 * dedicated metric type; for those the sample keeps its integer representation. With
 * `scale_by_unit` false no sample is scaled, which is the behavior before unit scaling was
 * introduced.
 * @param histogram the histogram the sample was recorded on.
 * @param value the recorded sample in the histogram's unit.
 * @param scale_by_unit whether to scale by the histogram's unit.
 * @return the sample in milliseconds when it was scaled, or nullopt when it is reported unchanged.
 */
std::optional<double> scaledTimerMilliseconds(const Stats::Histogram& histogram, uint64_t value,
                                              bool scale_by_unit);

/**
 * Implementation of Sink that writes to a UDP statsd address.
 */
class UdpStatsdSink : public Stats::Sink {
public:
  /**
   * Base interface for writing UDP datagrams.
   */
  class Writer : public ThreadLocal::ThreadLocalObject {
  public:
    virtual void write(const std::string& message) PURE;
    virtual void writeBuffer(Buffer::Instance& data) PURE;
  };

  UdpStatsdSink(ThreadLocal::SlotAllocator& tls, Network::Address::InstanceConstSharedPtr address,
                const bool use_tag, const std::string& prefix = getDefaultPrefix(),
                std::optional<uint64_t> buffer_size = std::nullopt,
                const Statsd::TagFormat& tag_format = Statsd::getDefaultTagFormat(),
                const bool scale_histogram_units = false);
  // For testing.
  UdpStatsdSink(ThreadLocal::SlotAllocator& tls, const std::shared_ptr<Writer>& writer,
                const bool use_tag, const std::string& prefix = getDefaultPrefix(),
                std::optional<uint64_t> buffer_size = std::nullopt,
                const Statsd::TagFormat& tag_format = Statsd::getDefaultTagFormat(),
                const bool scale_histogram_units = false);

  // Datagram size limit for batched histogram samples when max_bytes_per_datagram is not set. Small
  // enough to avoid IP fragmentation on any path.
  static constexpr uint64_t DEFAULT_SAMPLE_BATCH_BYTES = 512;

  // Stats::Sink
  void flush(Stats::MetricSnapshot& snapshot) override;
  void onHistogramComplete(const Stats::Histogram& histogram, uint64_t value) override;

  bool getUseTagForTest() { return use_tag_; }
  uint64_t getBufferSizeForTest() { return buffer_size_; }
  bool getScaleHistogramUnitsForTest() { return scale_histogram_units_; }
  uint64_t getSampleBatchBytesForTest() { return sample_batch_bytes_; }
  // Whether this thread's writer has a connected socket. Only valid for sinks built with an
  // address.
  bool writerConnectedForTest();
  const std::string& getPrefix() { return prefix_; }

  /**
   * Per-thread state of the sink: the thread's writer, plus the histogram samples recorded on the
   * thread during the current event-loop turn. Samples are joined by newlines, as flush() joins
   * lines, and sent as one datagram when the next sample would exceed the size limit, at the end of
   * the current dispatcher iteration, or when this object is destroyed, whichever comes first.
   */
  class ThreadLocalWriter : public ThreadLocal::ThreadLocalObject {
  public:
    /**
     * @param writer the writer of this thread.
     * @param dispatcher the dispatcher of this thread, used to send the batch at the end of the
     *        current iteration.
     * @param batch_bytes the datagram size limit for batched samples; 0 sends every sample as its
     *        own datagram.
     */
    ThreadLocalWriter(std::shared_ptr<Writer> writer, Event::Dispatcher& dispatcher,
                      uint64_t batch_bytes);
    ~ThreadLocalWriter() override;

    Writer& writer() { return *writer_; }
    void writeSample(const std::string& line);
    void flushSamples();

  private:
    const std::shared_ptr<Writer> writer_;
    Event::Dispatcher& dispatcher_;
    const uint64_t batch_bytes_;
    std::string pending_;
    Event::SchedulableCallbackPtr flush_cb_;
  };

private:
  /**
   * This is a simple UDP localhost writer for statsd messages.
   */
  class WriterImpl : public Writer {
  public:
    /**
     * @param address the statsd server address.
     * @param connect whether to connect the socket to an IP address, so that sends use the
     *        connected path. If connect() fails, the socket stays unconnected and every send names
     *        the address. Unix datagram sockets are never connected.
     */
    WriterImpl(Network::Address::InstanceConstSharedPtr address, bool connect);

    // Writer
    void write(const std::string& message) override;
    void writeBuffer(Buffer::Instance& data) override;

    bool connectedForTest() const { return io_handle_->wasConnected(); }

  private:
    // Owned here rather than referenced from the sink, so a writer that flushes from its thread's
    // teardown never depends on the sink's lifetime.
    const Network::Address::InstanceConstSharedPtr address_;
    const Network::IoHandlePtr io_handle_;
  };

  void flushBuffer(Buffer::OwnedImpl& buffer, Writer& writer) const;
  void writeBuffer(Buffer::OwnedImpl& buffer, Writer& writer, const std::string& data) const;

  template <class StatType, typename ValueType>
  const std::string buildMessage(const StatType& metric, ValueType value,
                                 const std::string& type) const;
  template <class StatType> const std::string getName(const StatType& metric) const;
  const std::string buildTagStr(const std::vector<Stats::Tag>& tags) const;

  const ThreadLocal::SlotSharedPtr tls_;
  const Network::Address::InstanceConstSharedPtr server_address_;
  const bool use_tag_;
  // Prefix for all flushed stats.
  const std::string prefix_;
  const uint64_t buffer_size_;
  const Statsd::TagFormat tag_format_;
  // Whether histogram samples are scaled to milliseconds according to the histogram's unit before
  // being reported as timers.
  const bool scale_histogram_units_;
  // Datagram size limit for the histogram samples batched per thread; 0 when batching is disabled.
  const uint64_t sample_batch_bytes_;
  // Whether each thread's writer connects its socket to the server address.
  const bool connect_socket_;
};

/**
 * Per thread implementation of a TCP stats flusher for statsd.
 */
class TcpStatsdSink : public Stats::Sink {
public:
  /**
   * This is a wrapper around the constructor to return StatusOr.
   */
  static absl::StatusOr<std::unique_ptr<TcpStatsdSink>>
  create(const LocalInfo::LocalInfo& local_info, const std::string& cluster_name,
         ThreadLocal::SlotAllocator& tls, Upstream::ClusterManager& cluster_manager,
         Stats::Scope& scope, const std::string& prefix = getDefaultPrefix(),
         const bool scale_histogram_units = false);

  // Stats::Sink
  void flush(Stats::MetricSnapshot& snapshot) override;
  void onHistogramComplete(const Stats::Histogram& histogram, uint64_t value) override;

  const std::string& getPrefix() { return prefix_; }
  bool getScaleHistogramUnitsForTest() { return scale_histogram_units_; }

protected:
  TcpStatsdSink(const LocalInfo::LocalInfo& local_info, const std::string& cluster_name,
                ThreadLocal::SlotAllocator& tls, Upstream::ClusterManager& cluster_manager,
                Stats::Scope& scope, absl::Status& creation_status,
                const std::string& prefix = getDefaultPrefix(),
                const bool scale_histogram_units = false);

private:
  // 16KiB intermediate buffer for flushing.
  static constexpr uint32_t FLUSH_SLICE_SIZE_BYTES = (1024 * 16);

  struct TlsSink : public ThreadLocal::ThreadLocalObject, public Network::ConnectionCallbacks {
    TlsSink(TcpStatsdSink& parent, Event::Dispatcher& dispatcher);
    ~TlsSink() override;

    void beginFlush(bool expect_empty_buffer, uint64_t slice_size = FLUSH_SLICE_SIZE_BYTES);
    void commonFlush(const std::string& name, uint64_t value, char stat_type);
    void flushCounter(const std::string& name, uint64_t delta);
    void flushGauge(const std::string& name, uint64_t value);
    void endFlush(bool do_write);
    void onTimespanComplete(const std::string& name, std::chrono::milliseconds ms);
    // For samples scaled to milliseconds from a finer unit.
    void onScaledTimespanComplete(const std::string& name, double milliseconds);
    void onPercentHistogramComplete(const std::string& name, float value);
    uint64_t usedBuffer() const;
    void write(Buffer::Instance& buffer);

    // Network::ConnectionCallbacks
    void onEvent(Network::ConnectionEvent event) override;
    void onAboveWriteBufferHighWatermark() override {}
    void onBelowWriteBufferLowWatermark() override {}

    TcpStatsdSink& parent_;
    Event::Dispatcher& dispatcher_;
    Network::ClientConnectionPtr connection_;
    Buffer::OwnedImpl buffer_;
    std::optional<Buffer::ReservationSingleSlice> current_buffer_reservation_;
    char* current_slice_mem_{};
  };

  // Somewhat arbitrary 16MiB limit for buffered stats.
  static constexpr uint32_t MAX_BUFFERED_STATS_BYTES = (1024 * 1024 * 16);

  // Prefix for all flushed stats.
  const std::string prefix_;
  // See UdpStatsdSink::scale_histogram_units_.
  const bool scale_histogram_units_;

  Upstream::ClusterInfoConstSharedPtr cluster_info_;
  ThreadLocal::SlotSharedPtr tls_;
  Upstream::ClusterManager& cluster_manager_;
  Stats::Counter& cx_overflow_stat_;
};

} // namespace Statsd
} // namespace Common
} // namespace StatSinks
} // namespace Extensions
} // namespace Envoy
