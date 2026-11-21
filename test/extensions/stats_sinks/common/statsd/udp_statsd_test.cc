#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "source/common/api/os_sys_calls_impl.h"
#include "source/common/network/address_impl.h"
#include "source/common/network/socket_impl.h"
#include "source/common/network/utility.h"
#include "source/extensions/stat_sinks/common/statsd/statsd.h"
#include "source/extensions/stat_sinks/common/statsd/tag_formats.h"

#include "test/mocks/event/mocks.h"
#include "test/mocks/stats/mocks.h"
#include "test/mocks/thread_local/mocks.h"
#include "test/test_common/environment.h"
#include "test/test_common/network_utility.h"
#include "test/test_common/test_runtime.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "spdlog/spdlog.h"

using testing::_;
using testing::InSequence;
using testing::NiceMock;

namespace Envoy {
namespace Extensions {
namespace StatSinks {
namespace Common {
namespace Statsd {
namespace {

class MockWriter : public UdpStatsdSink::Writer {
public:
  MOCK_METHOD(void, write, (const std::string& message));
  MOCK_METHOD(void, writeBuffer, (Buffer::Instance & buffer));

  void delegateBufferFake() {
    ON_CALL(*this, writeBuffer).WillByDefault([this](Buffer::Instance& buffer) {
      this->buffer_writes.push_back(buffer.toString());
    });
  }

  std::vector<std::string> buffer_writes;
};

constexpr absl::string_view BatchGuard = "envoy.reloadable_features.statsd_batch_histogram_samples";
constexpr absl::string_view ConnectGuard = "envoy.reloadable_features.statsd_connect_udp_socket";

// The tests below that check the text of single samples run with sample batching off, so that
// every sample is one write() as before batching existed; the batching tests come after them.
class PerSampleWrites {
public:
  PerSampleWrites() { scoped_runtime_.mergeValues({{std::string(BatchGuard), "false"}}); }

private:
  TestScopedRuntime scoped_runtime_;
};

// Skipping this test as Datagram sockets are not currently supported by UDS on Windows
#ifndef WIN32
// Regression test for https://github.com/envoyproxy/envoy/issues/8911
TEST(UdpOverUdsStatsdSinkTest, InitWithPipeAddress) {
  std::shared_ptr<Network::Address::PipeInstance> uds_address =
      *Network::Address::PipeInstance::create(
          TestEnvironment::unixDomainSocketPath("udstest.1.sock"));
  NiceMock<ThreadLocal::MockInstance> tls_;
  NiceMock<Stats::MockMetricSnapshot> snapshot;
  UdpStatsdSink sink(tls_, uds_address, false);

  NiceMock<Stats::MockCounter> counter;
  counter.name_ = "test_counter";
  counter.used_ = true;
  counter.latch_ = 1;
  snapshot.counters_.push_back({1, counter});

  // Flush before the server is running. This will fail.
  sink.flush(snapshot);

  // Start the server.
  Network::SocketImpl sock(Network::Socket::Type::Datagram, uds_address, nullptr, {});
  RELEASE_ASSERT(sock.setBlockingForTest(false).return_value_ != -1, "");
  sock.bind(uds_address);

  // Do the flush which should have somewhere to write now.
  sink.flush(snapshot);
  Buffer::OwnedImpl receive_buffer;
  sock.ioHandle().read(receive_buffer, 32);
  EXPECT_EQ("envoy.test_counter:1|c", receive_buffer.toString());
}
#endif

class UdpStatsdSinkTest : public testing::TestWithParam<Network::Address::IpVersion> {};
INSTANTIATE_TEST_SUITE_P(IpVersions, UdpStatsdSinkTest,
                         testing::ValuesIn(TestEnvironment::getIpVersionsForTest()),
                         TestUtility::ipTestParamsToString);

TEST_P(UdpStatsdSinkTest, InitWithIpAddress) {
  PerSampleWrites per_sample_writes;
  NiceMock<ThreadLocal::MockInstance> tls_;
  NiceMock<Stats::MockMetricSnapshot> snapshot;
  Network::Test::UdpSyncPeer server(GetParam());
  UdpStatsdSink sink(tls_, server.localAddress(), false);

  NiceMock<Stats::MockCounter> counter;
  counter.name_ = "test_counter";
  counter.used_ = true;
  counter.latch_ = 1;
  snapshot.counters_.push_back({1, counter});

  NiceMock<Stats::MockGauge> gauge;
  gauge.name_ = "test_gauge";
  gauge.value_ = 1;
  gauge.used_ = true;
  snapshot.gauges_.push_back(gauge);

  Stats::PrimitiveCounter host_counter;
  host_counter.add(3);
  Stats::PrimitiveCounterSnapshot host_counter_snap(host_counter);
  host_counter_snap.setName("test_host_counter");
  snapshot.host_counters_.push_back(host_counter_snap);

  Stats::PrimitiveGauge host_gauge;
  host_gauge.add(4);
  Stats::PrimitiveGaugeSnapshot host_gauge_snap(host_gauge);
  host_gauge_snap.setName("test_host_gauge");
  snapshot.host_gauges_.push_back(host_gauge_snap);

  sink.flush(snapshot);
  {
    Network::UdpRecvData data;
    server.recv(data);
    EXPECT_EQ("envoy.test_counter:1|c", data.buffer_->toString());
  }
  {
    Network::UdpRecvData data;
    server.recv(data);
    EXPECT_EQ("envoy.test_host_counter:3|c", data.buffer_->toString());
  }
  {
    Network::UdpRecvData data;
    server.recv(data);
    EXPECT_EQ("envoy.test_gauge:1|g", data.buffer_->toString());
  }
  {
    Network::UdpRecvData data;
    server.recv(data);
    EXPECT_EQ("envoy.test_host_gauge:4|g", data.buffer_->toString());
  }

  NiceMock<Stats::MockHistogram> timer;
  timer.name_ = "test_timer";
  sink.onHistogramComplete(timer, 5);
  Network::UdpRecvData data3;
  server.recv(data3);
  EXPECT_EQ("envoy.test_timer:5|ms", data3.buffer_->toString());

  tls_.shutdownThread();
}

class UdpStatsdSinkWithTagsTest : public testing::TestWithParam<Network::Address::IpVersion> {};
INSTANTIATE_TEST_SUITE_P(IpVersions, UdpStatsdSinkWithTagsTest,
                         testing::ValuesIn(TestEnvironment::getIpVersionsForTest()),
                         TestUtility::ipTestParamsToString);

TEST_P(UdpStatsdSinkWithTagsTest, InitWithIpAddress) {
  PerSampleWrites per_sample_writes;
  NiceMock<ThreadLocal::MockInstance> tls_;
  NiceMock<Stats::MockMetricSnapshot> snapshot;
  Network::Test::UdpSyncPeer server(GetParam());
  UdpStatsdSink sink(tls_, server.localAddress(), true);

  std::vector<Stats::Tag> tags = {Stats::Tag{"node", "test"}};
  NiceMock<Stats::MockCounter> counter;
  counter.name_ = "test_counter";
  counter.used_ = true;
  counter.latch_ = 1;
  counter.setTags(tags);
  snapshot.counters_.push_back({1, counter});

  NiceMock<Stats::MockGauge> gauge;
  gauge.name_ = "test_gauge";
  gauge.value_ = 1;
  gauge.used_ = true;
  gauge.setTags(tags);
  snapshot.gauges_.push_back(gauge);

  Stats::PrimitiveCounter host_counter;
  host_counter.add(3);
  Stats::PrimitiveCounterSnapshot host_counter_snap(host_counter);
  host_counter_snap.setTagExtractedName("test_host_counter");
  host_counter_snap.setTags(tags);
  snapshot.host_counters_.push_back(host_counter_snap);

  Stats::PrimitiveGauge host_gauge;
  host_gauge.add(4);
  Stats::PrimitiveGaugeSnapshot host_gauge_snap(host_gauge);
  host_gauge_snap.setTagExtractedName("test_host_gauge");
  host_gauge_snap.setTags(tags);
  snapshot.host_gauges_.push_back(host_gauge_snap);

  sink.flush(snapshot);
  {
    Network::UdpRecvData data;
    server.recv(data);
    EXPECT_EQ("envoy.test_counter:1|c|#node:test", data.buffer_->toString());
  }
  {
    Network::UdpRecvData data;
    server.recv(data);
    EXPECT_EQ("envoy.test_host_counter:3|c|#node:test", data.buffer_->toString());
  }
  {
    Network::UdpRecvData data;
    server.recv(data);
    EXPECT_EQ("envoy.test_gauge:1|g|#node:test", data.buffer_->toString());
  }
  {
    Network::UdpRecvData data;
    server.recv(data);
    EXPECT_EQ("envoy.test_host_gauge:4|g|#node:test", data.buffer_->toString());
  }

  NiceMock<Stats::MockHistogram> timer;
  timer.name_ = "test_timer";
  timer.setTags(tags);
  sink.onHistogramComplete(timer, 5);
  Network::UdpRecvData data3;
  server.recv(data3);
  EXPECT_EQ("envoy.test_timer:5|ms|#node:test", data3.buffer_->toString());

  tls_.shutdownThread();
}

TEST(UdpStatsdSinkTest, CheckActualStats) {
  PerSampleWrites per_sample_writes;
  NiceMock<Stats::MockMetricSnapshot> snapshot;
  auto writer_ptr = std::make_shared<NiceMock<MockWriter>>();
  writer_ptr->delegateBufferFake();
  NiceMock<ThreadLocal::MockInstance> tls_;
  UdpStatsdSink sink(tls_, writer_ptr, false, getDefaultPrefix(), 1024);

  NiceMock<Stats::MockCounter> counter;
  counter.name_ = "test_counter";
  counter.used_ = true;
  counter.latch_ = 1;
  snapshot.counters_.push_back({1, counter});

  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr), writeBuffer(_));
  sink.flush(snapshot);
  EXPECT_EQ(writer_ptr->buffer_writes.size(), 1);
  EXPECT_EQ(writer_ptr->buffer_writes.at(0), "envoy.test_counter:1|c");
  counter.used_ = false;

  NiceMock<Stats::MockGauge> gauge;
  gauge.name_ = "test_gauge";
  gauge.value_ = 1;
  gauge.used_ = true;
  snapshot.gauges_.push_back(gauge);

  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr), writeBuffer(_));
  sink.flush(snapshot);
  EXPECT_EQ(writer_ptr->buffer_writes.size(), 2);
  EXPECT_EQ(writer_ptr->buffer_writes.at(1), "envoy.test_gauge:1|g");

  NiceMock<Stats::MockHistogram> timer;
  timer.name_ = "test_timer";
  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr),
              write("envoy.test_timer:5|ms"));
  sink.onHistogramComplete(timer, 5);

  tls_.shutdownThread();
}

TEST(UdpStatsdSinkTest, CheckMetricLargerThanBuffer) {
  NiceMock<Stats::MockMetricSnapshot> snapshot;
  auto writer_ptr = std::make_shared<NiceMock<MockWriter>>();
  writer_ptr->delegateBufferFake();
  NiceMock<ThreadLocal::MockInstance> tls_;
  uint64_t buffer_size = 4;
  UdpStatsdSink sink(tls_, writer_ptr, false, getDefaultPrefix(), buffer_size);

  NiceMock<Stats::MockCounter> counter;
  counter.name_ = "test_counter";
  counter.used_ = true;
  counter.latch_ = 1;
  snapshot.counters_.push_back({1, counter});

  // Expect the metric to skip the buffer
  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr),
              write("envoy.test_counter:1|c"));
  sink.flush(snapshot);
  counter.used_ = false;

  NiceMock<Stats::MockGauge> gauge;
  gauge.name_ = "test_gauge";
  gauge.value_ = 1;
  gauge.used_ = true;
  snapshot.gauges_.push_back(gauge);

  // Expect the metric to skip the buffer
  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr),
              write("envoy.test_gauge:1|g"));
  sink.flush(snapshot);

  tls_.shutdownThread();
}

TEST(UdpStatsdSinkTest, CheckBufferedWritesWithinBufferSize) {
  NiceMock<Stats::MockMetricSnapshot> snapshot;
  auto writer_ptr = std::make_shared<NiceMock<MockWriter>>();
  writer_ptr->delegateBufferFake();
  NiceMock<ThreadLocal::MockInstance> tls_;
  uint64_t buffer_size = 1024;
  UdpStatsdSink sink(tls_, writer_ptr, false, getDefaultPrefix(), buffer_size);

  NiceMock<Stats::MockCounter> counter;
  counter.name_ = "test_counter";
  counter.used_ = true;
  counter.latch_ = 1;
  snapshot.counters_.push_back({1, counter});

  NiceMock<Stats::MockGauge> gauge;
  gauge.name_ = "test_gauge";
  gauge.value_ = 1;
  gauge.used_ = true;
  snapshot.gauges_.push_back(gauge);

  // Expect both metrics to be present in single write
  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr), writeBuffer(_));
  sink.flush(snapshot);
  EXPECT_EQ(writer_ptr->buffer_writes.size(), 1);
  EXPECT_EQ(writer_ptr->buffer_writes.at(0), "envoy.test_counter:1|c\nenvoy.test_gauge:1|g");

  tls_.shutdownThread();
}

TEST(UdpStatsdSinkTest, CheckBufferedWritesExceedingBufferSize) {
  NiceMock<Stats::MockMetricSnapshot> snapshot;
  auto writer_ptr = std::make_shared<NiceMock<MockWriter>>();
  writer_ptr->delegateBufferFake();
  NiceMock<ThreadLocal::MockInstance> tls_;
  uint64_t buffer_size = 64;
  UdpStatsdSink sink(tls_, writer_ptr, false, getDefaultPrefix(), buffer_size);

  NiceMock<Stats::MockCounter> counter_1;
  counter_1.name_ = "test_counter_1";
  counter_1.used_ = true;
  counter_1.latch_ = 1;
  snapshot.counters_.push_back({1, counter_1});

  NiceMock<Stats::MockCounter> counter_2;
  counter_2.name_ = "test_counter_2";
  counter_2.used_ = true;
  counter_2.latch_ = 1;
  snapshot.counters_.push_back({1, counter_2});

  NiceMock<Stats::MockGauge> gauge;
  gauge.name_ = "test_gauge";
  gauge.value_ = 1;
  gauge.used_ = true;
  snapshot.gauges_.push_back(gauge);

  // Expect both metrics to be present in single write
  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr), writeBuffer(_))
      .Times(2);
  sink.flush(snapshot);
  EXPECT_EQ(writer_ptr->buffer_writes.size(), 2);
  EXPECT_EQ(writer_ptr->buffer_writes.at(0), "envoy.test_counter_1:1|c\nenvoy.test_counter_2:1|c");
  EXPECT_EQ(writer_ptr->buffer_writes.at(1), "envoy.test_gauge:1|g");

  tls_.shutdownThread();
}

TEST(UdpStatsdSinkTest, CheckActualStatsWithCustomPrefix) {
  NiceMock<Stats::MockMetricSnapshot> snapshot;
  auto writer_ptr = std::make_shared<NiceMock<MockWriter>>();
  writer_ptr->delegateBufferFake();
  NiceMock<ThreadLocal::MockInstance> tls_;
  UdpStatsdSink sink(tls_, writer_ptr, false, "test_prefix", 1024);

  NiceMock<Stats::MockCounter> counter;
  counter.name_ = "test_counter";
  counter.used_ = true;
  counter.latch_ = 1;
  snapshot.counters_.push_back({1, counter});

  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr), writeBuffer(_));
  sink.flush(snapshot);
  EXPECT_EQ(writer_ptr->buffer_writes.size(), 1);
  EXPECT_EQ(writer_ptr->buffer_writes.at(0), "test_prefix.test_counter:1|c");
  counter.used_ = false;

  tls_.shutdownThread();
}

TEST(UdpStatsdSinkTest, SiSuffix) {
  PerSampleWrites per_sample_writes;
  NiceMock<Stats::MockMetricSnapshot> snapshot;
  auto writer_ptr = std::make_shared<NiceMock<MockWriter>>();
  NiceMock<ThreadLocal::MockInstance> tls_;
  UdpStatsdSink sink(tls_, writer_ptr, false, getDefaultPrefix(), std::nullopt,
                     getDefaultTagFormat(), /*scale_histogram_units=*/true);

  NiceMock<Stats::MockHistogram> items;
  items.name_ = "items";
  items.unit_ = Stats::Histogram::Unit::Unspecified;

  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr),
              write("envoy.items:1|ms"));
  sink.onHistogramComplete(items, 1);
  // Unscaled samples keep their integer representation however large they are.
  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr),
              write("envoy.items:1234567|ms"));
  sink.onHistogramComplete(items, 1234567);

  NiceMock<Stats::MockHistogram> information;
  information.name_ = "information";
  information.unit_ = Stats::Histogram::Unit::Bytes;

  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr),
              write("envoy.information:2|ms"));
  sink.onHistogramComplete(information, 2);
  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr),
              write("envoy.information:2097152|ms"));
  sink.onHistogramComplete(information, 2097152);

  NiceMock<Stats::MockHistogram> duration_micro;
  duration_micro.name_ = "duration";
  duration_micro.unit_ = Stats::Histogram::Unit::Microseconds;

  // Microseconds are scaled to milliseconds.
  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr),
              write("envoy.duration:0.003|ms"));
  sink.onHistogramComplete(duration_micro, 3);
  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr),
              write("envoy.duration:1.5|ms"));
  sink.onHistogramComplete(duration_micro, 1500);
  // Large scaled samples keep full precision and never use scientific notation.
  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr),
              write("envoy.duration:1234567.891|ms"));
  sink.onHistogramComplete(duration_micro, 1234567891);

  NiceMock<Stats::MockHistogram> duration_nano;
  duration_nano.name_ = "duration";
  duration_nano.unit_ = Stats::Histogram::Unit::Nanoseconds;

  // Nanoseconds are scaled to milliseconds without losing the sub-microsecond part.
  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr),
              write("envoy.duration:0.0407|ms"));
  sink.onHistogramComplete(duration_nano, 40700);
  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr),
              write("envoy.duration:3.943277|ms"));
  sink.onHistogramComplete(duration_nano, 3943277);

  NiceMock<Stats::MockHistogram> duration_milli;
  duration_milli.name_ = "duration";
  duration_milli.unit_ = Stats::Histogram::Unit::Milliseconds;

  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr),
              write("envoy.duration:4|ms"));
  sink.onHistogramComplete(duration_milli, 4);

  tls_.shutdownThread();
}

// Unit scaling is off by default: every sample is reported unscaled with an ms suffix.
TEST(UdpStatsdSinkTest, HistogramUnitScalingOffByDefault) {
  PerSampleWrites per_sample_writes;
  auto writer_ptr = std::make_shared<NiceMock<MockWriter>>();
  NiceMock<ThreadLocal::MockInstance> tls_;
  UdpStatsdSink sink(tls_, writer_ptr, false);

  NiceMock<Stats::MockHistogram> duration_micro;
  duration_micro.name_ = "duration";
  duration_micro.unit_ = Stats::Histogram::Unit::Microseconds;

  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr),
              write("envoy.duration:1500|ms"));
  sink.onHistogramComplete(duration_micro, 1500);

  NiceMock<Stats::MockHistogram> duration_nano;
  duration_nano.name_ = "duration";
  duration_nano.unit_ = Stats::Histogram::Unit::Nanoseconds;

  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr),
              write("envoy.duration:40700|ms"));
  sink.onHistogramComplete(duration_nano, 40700);

  tls_.shutdownThread();
}

TEST(UdpStatsdSinkTest, ScaledPercent) {
  PerSampleWrites per_sample_writes;
  NiceMock<Stats::MockMetricSnapshot> snapshot;
  auto writer_ptr = std::make_shared<NiceMock<MockWriter>>();
  NiceMock<ThreadLocal::MockInstance> tls_;
  UdpStatsdSink sink(tls_, writer_ptr, false);

  NiceMock<Stats::MockHistogram> items;
  items.name_ = "items";
  items.unit_ = Stats::Histogram::Unit::Percent;

  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr),
              write("envoy.items:0.5|h"));
  sink.onHistogramComplete(items, Stats::Histogram::PercentScale / 2);

  tls_.shutdownThread();
}

TEST(UdpStatsdSinkWithTagsTest, CheckActualStats) {
  PerSampleWrites per_sample_writes;
  NiceMock<Stats::MockMetricSnapshot> snapshot;
  auto writer_ptr = std::make_shared<NiceMock<MockWriter>>();
  writer_ptr->delegateBufferFake();
  NiceMock<ThreadLocal::MockInstance> tls_;
  UdpStatsdSink sink(tls_, writer_ptr, true, getDefaultPrefix(), 1024);

  std::vector<Stats::Tag> tags = {Stats::Tag{"key1", "value1"}, Stats::Tag{"key2", "value2"}};
  NiceMock<Stats::MockCounter> counter;
  counter.name_ = "test_counter";
  counter.used_ = true;
  counter.latch_ = 1;
  counter.setTags(tags);
  snapshot.counters_.push_back({1, counter});

  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr), writeBuffer(_));
  sink.flush(snapshot);
  EXPECT_EQ(writer_ptr->buffer_writes.size(), 1);
  EXPECT_EQ(writer_ptr->buffer_writes.at(0), "envoy.test_counter:1|c|#key1:value1,key2:value2");
  counter.used_ = false;

  NiceMock<Stats::MockGauge> gauge;
  gauge.name_ = "test_gauge";
  gauge.value_ = 1;
  gauge.used_ = true;
  gauge.setTags(tags);
  snapshot.gauges_.push_back(gauge);

  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr), writeBuffer(_));
  sink.flush(snapshot);
  EXPECT_EQ(writer_ptr->buffer_writes.size(), 2);
  EXPECT_EQ(writer_ptr->buffer_writes.at(1), "envoy.test_gauge:1|g|#key1:value1,key2:value2");

  NiceMock<Stats::MockHistogram> timer;
  timer.name_ = "test_timer";
  timer.setTags(tags);
  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr),
              write("envoy.test_timer:5|ms|#key1:value1,key2:value2"));
  sink.onHistogramComplete(timer, 5);

  tls_.shutdownThread();
}

TEST(UdpStatsdSinkWithTagsTest, SiSuffix) {
  PerSampleWrites per_sample_writes;
  NiceMock<Stats::MockMetricSnapshot> snapshot;
  auto writer_ptr = std::make_shared<NiceMock<MockWriter>>();
  NiceMock<ThreadLocal::MockInstance> tls_;
  UdpStatsdSink sink(tls_, writer_ptr, true, getDefaultPrefix(), std::nullopt,
                     getDefaultTagFormat(), /*scale_histogram_units=*/true);

  std::vector<Stats::Tag> tags = {Stats::Tag{"key1", "value1"}, Stats::Tag{"key2", "value2"}};

  NiceMock<Stats::MockHistogram> items;
  items.name_ = "items";
  items.unit_ = Stats::Histogram::Unit::Unspecified;
  items.setTags(tags);

  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr),
              write("envoy.items:1|ms|#key1:value1,key2:value2"));
  sink.onHistogramComplete(items, 1);

  NiceMock<Stats::MockHistogram> information;
  information.name_ = "information";
  information.unit_ = Stats::Histogram::Unit::Bytes;
  information.setTags(tags);

  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr),
              write("envoy.information:2|ms|#key1:value1,key2:value2"));
  sink.onHistogramComplete(information, 2);

  NiceMock<Stats::MockHistogram> duration_micro;
  duration_micro.name_ = "duration";
  duration_micro.unit_ = Stats::Histogram::Unit::Microseconds;
  duration_micro.setTags(tags);

  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr),
              write("envoy.duration:0.003|ms|#key1:value1,key2:value2"));
  sink.onHistogramComplete(duration_micro, 3);

  NiceMock<Stats::MockHistogram> duration_nano;
  duration_nano.name_ = "duration";
  duration_nano.unit_ = Stats::Histogram::Unit::Nanoseconds;
  duration_nano.setTags(tags);

  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr),
              write("envoy.duration:0.0407|ms|#key1:value1,key2:value2"));
  sink.onHistogramComplete(duration_nano, 40700);

  NiceMock<Stats::MockHistogram> duration_milli;
  duration_milli.name_ = "duration";
  duration_milli.unit_ = Stats::Histogram::Unit::Milliseconds;
  duration_milli.setTags(tags);

  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr),
              write("envoy.duration:4|ms|#key1:value1,key2:value2"));
  sink.onHistogramComplete(duration_milli, 4);

  tls_.shutdownThread();
}

TEST(UdpStatsdSinkWithTagsTest, GraphiteTagSyntax) {
  PerSampleWrites per_sample_writes;
  NiceMock<Stats::MockMetricSnapshot> snapshot;
  auto writer_ptr = std::make_shared<NiceMock<MockWriter>>();
  writer_ptr->delegateBufferFake();
  NiceMock<ThreadLocal::MockInstance> tls_;
  UdpStatsdSink sink(tls_, writer_ptr, true, getDefaultPrefix(), 1024, getGraphiteTagFormat());

  std::vector<Stats::Tag> tags = {Stats::Tag{"key1", "value1"}, Stats::Tag{"key2", "value2"}};
  NiceMock<Stats::MockCounter> counter;
  counter.name_ = "test_counter";
  counter.used_ = true;
  counter.latch_ = 1;
  counter.setTags(tags);
  snapshot.counters_.push_back({1, counter});

  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr), writeBuffer(_));
  sink.flush(snapshot);
  EXPECT_EQ(writer_ptr->buffer_writes.size(), 1);
  EXPECT_EQ(writer_ptr->buffer_writes.at(0), "envoy.test_counter;key1=value1;key2=value2:1|c");
  counter.used_ = false;

  NiceMock<Stats::MockGauge> gauge;
  gauge.name_ = "test_gauge";
  gauge.value_ = 1;
  gauge.used_ = true;
  gauge.setTags(tags);
  snapshot.gauges_.push_back(gauge);

  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr), writeBuffer(_));
  sink.flush(snapshot);
  EXPECT_EQ(writer_ptr->buffer_writes.size(), 2);
  EXPECT_EQ(writer_ptr->buffer_writes.at(1), "envoy.test_gauge;key1=value1;key2=value2:1|g");

  NiceMock<Stats::MockHistogram> timer;
  timer.name_ = "test_timer";
  timer.setTags(tags);
  EXPECT_CALL(*std::dynamic_pointer_cast<NiceMock<MockWriter>>(writer_ptr),
              write("envoy.test_timer;key1=value1;key2=value2:5|ms"));
  sink.onHistogramComplete(timer, 5);

  tls_.shutdownThread();
}

// Histogram samples recorded on a thread during one event-loop turn.

class UdpStatsdSinkBatchTest : public testing::Test {
protected:
  UdpStatsdSinkBatchTest() : writer_(std::make_shared<NiceMock<MockWriter>>()) {}

  // The flush callback is created on the first sample; this captures it.
  NiceMock<Event::MockSchedulableCallback>* expectFlushCallback() {
    return new NiceMock<Event::MockSchedulableCallback>(&tls_.dispatcher_);
  }

  void sample(UdpStatsdSink& sink, const std::string& name, uint64_t value) {
    NiceMock<Stats::MockHistogram> histogram;
    histogram.name_ = name;
    sink.onHistogramComplete(histogram, value);
  }

  NiceMock<ThreadLocal::MockInstance> tls_;
  std::shared_ptr<NiceMock<MockWriter>> writer_;
};

TEST_F(UdpStatsdSinkBatchTest, BatchLimit) {
  {
    UdpStatsdSink sink(tls_, writer_, false);
    EXPECT_EQ(UdpStatsdSink::DEFAULT_SAMPLE_BATCH_BYTES, sink.getSampleBatchBytesForTest());
  }
  {
    // max_bytes_per_datagram is the limit when it is set.
    UdpStatsdSink sink(tls_, writer_, false, getDefaultPrefix(), 1432);
    EXPECT_EQ(1432, sink.getSampleBatchBytesForTest());
  }
  {
    PerSampleWrites per_sample_writes;
    UdpStatsdSink sink(tls_, writer_, false, getDefaultPrefix(), 1432);
    EXPECT_EQ(0, sink.getSampleBatchBytesForTest());
  }
}

TEST_F(UdpStatsdSinkBatchTest, SamplesOfOneTurnAreOneDatagram) {
  UdpStatsdSink sink(tls_, writer_, false);
  auto* flush_cb = expectFlushCallback();

  // Nothing is sent while the turn runs.
  EXPECT_CALL(*writer_, write(_)).Times(0);
  EXPECT_CALL(*flush_cb, scheduleCallbackCurrentIteration()).Times(3);
  sample(sink, "upstream_rq_time", 5);
  EXPECT_TRUE(flush_cb->enabled_);
  sample(sink, "external.upstream_rq_time", 5);
  sample(sink, "downstream_rq_time", 7);
  testing::Mock::VerifyAndClearExpectations(writer_.get());
  testing::Mock::VerifyAndClearExpectations(flush_cb);

  // At the end of the turn the three lines go out as one datagram, in recording order.
  EXPECT_CALL(*writer_, write("envoy.upstream_rq_time:5|ms\n"
                              "envoy.external.upstream_rq_time:5|ms\n"
                              "envoy.downstream_rq_time:7|ms"));
  flush_cb->invokeCallback();
  testing::Mock::VerifyAndClearExpectations(writer_.get());

  // The next turn starts a new batch with the same callback.
  EXPECT_CALL(*writer_, write("envoy.upstream_rq_time:6|ms"));
  sample(sink, "upstream_rq_time", 6);
  EXPECT_TRUE(flush_cb->enabled_);
  flush_cb->invokeCallback();
  testing::Mock::VerifyAndClearExpectations(writer_.get());

  // A callback with nothing pending sends nothing, and neither does shutdown.
  EXPECT_CALL(*writer_, write(_)).Times(0);
  flush_cb->enabled_ = true;
  flush_cb->invokeCallback();
  tls_.shutdownThread();
}

TEST_F(UdpStatsdSinkBatchTest, FullBatchIsSentBeforeTheTurnEnds) {
  // Each line is 21 bytes; two lines and a newline (43) fit in 64, a third (65) does not.
  UdpStatsdSink sink(tls_, writer_, false, getDefaultPrefix(), 64);
  auto* flush_cb = expectFlushCallback();

  InSequence s;
  EXPECT_CALL(*writer_, write("envoy.test_timer:1|ms\nenvoy.test_timer:2|ms"));
  EXPECT_CALL(*writer_, write("envoy.test_timer:3|ms"));
  sample(sink, "test_timer", 1);
  sample(sink, "test_timer", 2);
  sample(sink, "test_timer", 3);
  flush_cb->invokeCallback();
  tls_.shutdownThread();
}

TEST_F(UdpStatsdSinkBatchTest, ExactFit) {
  // Two 21-byte lines and their newline are exactly 43 bytes: one datagram.
  UdpStatsdSink sink(tls_, writer_, false, getDefaultPrefix(), 43);
  auto* flush_cb = expectFlushCallback();

  EXPECT_CALL(*writer_, write("envoy.test_timer:1|ms\nenvoy.test_timer:2|ms"));
  sample(sink, "test_timer", 1);
  sample(sink, "test_timer", 2);
  flush_cb->invokeCallback();
  tls_.shutdownThread();
}

TEST_F(UdpStatsdSinkBatchTest, LineAtTheLimitIsSentAloneInOrder) {
  // "envoy.a:1|ms" is 12 bytes and batched; "envoy.test_timer:2|ms" is 21 bytes, at the limit.
  UdpStatsdSink sink(tls_, writer_, false, getDefaultPrefix(), 21);
  auto* flush_cb = expectFlushCallback();

  InSequence s;
  EXPECT_CALL(*writer_, write("envoy.a:1|ms"));
  EXPECT_CALL(*writer_, write("envoy.test_timer:2|ms"));
  EXPECT_CALL(*writer_, write("envoy.a:3|ms"));
  sample(sink, "a", 1);
  sample(sink, "test_timer", 2);
  sample(sink, "a", 3);
  flush_cb->invokeCallback();
  tls_.shutdownThread();
}

TEST_F(UdpStatsdSinkBatchTest, ThreadShutdownSendsPendingSamples) {
  UdpStatsdSink sink(tls_, writer_, false);
  expectFlushCallback();

  sample(sink, "test_timer", 1);
  sample(sink, "test_timer", 2);
  EXPECT_CALL(*writer_, write("envoy.test_timer:1|ms\nenvoy.test_timer:2|ms"));
  tls_.shutdownThread();
  testing::Mock::VerifyAndClearExpectations(writer_.get());
}

TEST_F(UdpStatsdSinkBatchTest, SinkDestructionSendsPendingSamples) {
  {
    UdpStatsdSink sink(tls_, writer_, false);
    expectFlushCallback();
    sample(sink, "test_timer", 1);
    EXPECT_CALL(*writer_, write("envoy.test_timer:1|ms"));
  }
  testing::Mock::VerifyAndClearExpectations(writer_.get());
}

TEST_F(UdpStatsdSinkBatchTest, FlushIsNotBatchedWithSamples) {
  UdpStatsdSink sink(tls_, writer_, false, getDefaultPrefix(), 1024);
  writer_->delegateBufferFake();
  auto* flush_cb = expectFlushCallback();

  sample(sink, "test_timer", 1);
  NiceMock<Stats::MockMetricSnapshot> snapshot;
  NiceMock<Stats::MockCounter> counter;
  counter.name_ = "test_counter";
  counter.used_ = true;
  counter.latch_ = 1;
  snapshot.counters_.push_back({1, counter});
  sink.flush(snapshot);
  ASSERT_EQ(1, writer_->buffer_writes.size());
  EXPECT_EQ("envoy.test_counter:1|c", writer_->buffer_writes[0]);

  EXPECT_CALL(*writer_, write("envoy.test_timer:1|ms"));
  flush_cb->invokeCallback();
  tls_.shutdownThread();
}

TEST_F(UdpStatsdSinkBatchTest, BatchingOffWritesEverySample) {
  PerSampleWrites per_sample_writes;
  UdpStatsdSink sink(tls_, writer_, false);
  EXPECT_CALL(tls_.dispatcher_, createSchedulableCallback_(_)).Times(0);

  InSequence s;
  EXPECT_CALL(*writer_, write("envoy.test_timer:1|ms"));
  EXPECT_CALL(*writer_, write("envoy.test_timer:2|ms"));
  sample(sink, "test_timer", 1);
  sample(sink, "test_timer", 2);
  tls_.shutdownThread();
}

// A real UDP socket and a real dispatcher: count the datagrams the receiver gets.
class UdpStatsdSinkBatchSocketTest : public testing::TestWithParam<Network::Address::IpVersion> {
protected:
  UdpStatsdSinkBatchSocketTest()
      : api_(Api::createApiForTest()), dispatcher_(api_->allocateDispatcher("test_thread")),
        server_(GetParam()) {
    tls_.setDispatcher(dispatcher_.get());
  }

  void sample(UdpStatsdSink& sink, const std::string& name, uint64_t value) {
    NiceMock<Stats::MockHistogram> histogram;
    histogram.name_ = name;
    sink.onHistogramComplete(histogram, value);
  }

  std::string recv() {
    Network::UdpRecvData data;
    server_.recv(data);
    return data.buffer_->toString();
  }

  Api::ApiPtr api_;
  Event::DispatcherPtr dispatcher_;
  Network::Test::UdpSyncPeer server_;
  NiceMock<ThreadLocal::MockInstance> tls_;
};
INSTANTIATE_TEST_SUITE_P(IpVersions, UdpStatsdSinkBatchSocketTest,
                         testing::ValuesIn(TestEnvironment::getIpVersionsForTest()),
                         TestUtility::ipTestParamsToString);

TEST_P(UdpStatsdSinkBatchSocketTest, OneDatagramPerTurn) {
  UdpStatsdSink sink(tls_, server_.localAddress(), false);
  EXPECT_TRUE(sink.writerConnectedForTest());

  sample(sink, "upstream_rq_time", 5);
  sample(sink, "external.upstream_rq_time", 5);
  sample(sink, "downstream_rq_time", 7);
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);
  // The next turn's sample is the next datagram, so the first turn sent exactly one.
  sample(sink, "upstream_rq_time", 6);
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);

  EXPECT_EQ("envoy.upstream_rq_time:5|ms\nenvoy.external.upstream_rq_time:5|ms\n"
            "envoy.downstream_rq_time:7|ms",
            recv());
  EXPECT_EQ("envoy.upstream_rq_time:6|ms", recv());

  // A pending sample is sent when the thread shuts down.
  sample(sink, "downstream_rq_time", 8);
  tls_.shutdownThread();
  EXPECT_EQ("envoy.downstream_rq_time:8|ms", recv());
}

TEST_P(UdpStatsdSinkBatchSocketTest, GuardsOff) {
  TestScopedRuntime scoped_runtime;
  scoped_runtime.mergeValues(
      {{std::string(BatchGuard), "false"}, {std::string(ConnectGuard), "false"}});
  UdpStatsdSink sink(tls_, server_.localAddress(), false);
  EXPECT_FALSE(sink.writerConnectedForTest());

  sample(sink, "upstream_rq_time", 5);
  sample(sink, "downstream_rq_time", 7);
  EXPECT_EQ("envoy.upstream_rq_time:5|ms", recv());
  EXPECT_EQ("envoy.downstream_rq_time:7|ms", recv());
  tls_.shutdownThread();
}

} // namespace
} // namespace Statsd
} // namespace Common
} // namespace StatSinks
} // namespace Extensions
} // namespace Envoy
