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
#include "source/common/common/logger.h"
#include "source/common/common/macros.h"
#include "source/common/network/io_socket_handle_impl.h"
#include "source/extensions/stat_sinks/common/statsd/tag_formats.h"

namespace Envoy {
namespace Extensions {
namespace StatSinks {
namespace Common {
namespace Statsd {

inline const std::string& getDefaultPrefix() { CONSTRUCT_ON_FIRST_USE(std::string, "envoy"); }

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
                const bool scale_histogram_units = false,
                const bool batch_histogram_samples = false);
  // For testing.
  UdpStatsdSink(ThreadLocal::SlotAllocator& tls, const std::shared_ptr<Writer>& writer,
                const bool use_tag, const std::string& prefix = getDefaultPrefix(),
                std::optional<uint64_t> buffer_size = std::nullopt,
                const Statsd::TagFormat& tag_format = Statsd::getDefaultTagFormat(),
                const bool scale_histogram_units = false,
                const bool batch_histogram_samples = false);

  // The largest UDP payload: 65,535 minus the IP and UDP headers (IPv4 65,507, IPv6 65,527). A
  // datagram size limit above it is lowered to it, since a larger datagram fails to send.
  static constexpr uint64_t MAX_UDP_PAYLOAD_IPV4 = 65507;
  static constexpr uint64_t MAX_UDP_PAYLOAD_IPV6 = 65527;

  // Stats::Sink
  void flush(Stats::MetricSnapshot& snapshot) override;
  void onHistogramComplete(const Stats::Histogram& histogram, uint64_t value) override;

  bool getUseTagForTest() { return use_tag_; }
  uint64_t getBufferSizeForTest() { return buffer_size_; }
  bool getScaleHistogramUnitsForTest() { return scale_histogram_units_; }
  // Whether this thread's writer sends over a connected socket. False for sinks built with a
  // writer for testing.
  bool writerConnectedForTest();
  // The datagram size limit for the histogram samples batched per thread; 0 when they are not
  // batched.
  uint64_t getSampleBatchBytesForTest() { return sample_batch_bytes_; }
  const std::string& getPrefix() { return prefix_; }

private:
  /**
   * The state of the sink on one thread: the thread's writer, and the histogram samples the
   * thread recorded during the current iteration of its event loop. Samples are joined by
   * newlines, as flush() joins lines, and sent as one datagram when the next sample would exceed
   * the limit, at the end of the current dispatcher iteration, before the stats of a flush on this
   * thread, or when this object is destroyed, whichever comes first.
   */
  class ThreadLocalWriter : public ThreadLocal::ThreadLocalObject {
  public:
    /**
     * @param writer the writer of this thread.
     * @param dispatcher the dispatcher of this thread.
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

  /**
   * The UDP writer of one thread. For an IP address it connects its socket, so that every send
   * reuses the route and source address resolved once instead of naming the destination (see
   * send() for how errors on the connected socket are handled). Unix datagram sockets are never
   * connected: a connected one is tied to one receiver socket and fails for good once that
   * receiver re-binds its path. The writer owns the address, so it never depends on the sink's
   * lifetime.
   */
  class WriterImpl : public Writer, Logger::Loggable<Logger::Id::stats> {
  public:
    WriterImpl(Network::Address::InstanceConstSharedPtr address, bool connect);

    // Writer
    void write(const std::string& message) override;
    void writeBuffer(Buffer::Instance& data) override;

    bool connected() const { return connected_; }

  private:
    void send(Buffer::RawSlice* slices, uint64_t num_slices);

    const Network::Address::InstanceConstSharedPtr address_;
    Network::IoHandlePtr io_handle_;
    bool connected_{false};
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
  // The datagram size limit for the histogram samples batched per thread; 0 when they are not
  // batched.
  const uint64_t sample_batch_bytes_;
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
