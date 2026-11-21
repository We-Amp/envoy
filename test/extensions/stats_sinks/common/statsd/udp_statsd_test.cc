#ifndef WIN32
#include <unistd.h>
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "source/common/api/os_sys_calls_impl.h"
#include "source/common/common/thread.h"
#include "source/common/common/utility.h"
#include "source/common/network/address_impl.h"
#include "source/common/network/socket_impl.h"
#include "source/common/network/utility.h"
#include "source/common/thread_local/thread_local_impl.h"
#include "source/extensions/stat_sinks/common/statsd/statsd.h"
#include "source/extensions/stat_sinks/common/statsd/tag_formats.h"

#include "test/mocks/event/mocks.h"
#include "test/mocks/stats/mocks.h"
#include "test/mocks/thread_local/mocks.h"
#include "test/test_common/environment.h"
#include "test/test_common/network_utility.h"
#include "test/test_common/test_runtime.h"
#include "test/test_common/thread_factory_for_test.h"
#include "test/test_common/threadsafe_singleton_injector.h"
#include "test/test_common/utility.h"

#include "absl/strings/str_cat.h"
#include "absl/synchronization/notification.h"
#include "absl/time/clock.h"
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

// The connected socket of the UDP writer.

constexpr absl::string_view ConnectGuard = "envoy.reloadable_features.statsd_udp_connect_socket";

std::string recvString(Network::Test::UdpSyncPeer& server) {
  Network::UdpRecvData data;
  server.recv(data);
  return data.buffer_->toString();
}

// Reads one datagram from a socket, waiting up to two seconds for it to arrive.
std::string readDatagram(Network::Socket& socket) {
  Buffer::OwnedImpl buffer;
  for (int i = 0; i < 200 && buffer.length() == 0; i++) {
    if (!socket.ioHandle().read(buffer, 65536).ok() || buffer.length() == 0) {
      absl::SleepFor(absl::Milliseconds(10));
    }
  }
  return buffer.toString();
}

void sample(UdpStatsdSink& sink, const std::string& name, uint64_t value) {
  NiceMock<Stats::MockHistogram> histogram;
  histogram.name_ = name;
  sink.onHistogramComplete(histogram, value);
}

// Real system calls, except that connect and the connected sends can be made to fail. A connected
// send is send() for one slice and writev() for several; both are counted as writev_calls_.
class FaultySysCalls : public Api::OsSysCallsImpl {
public:
  Api::SysCallSizeResult writev(os_fd_t fd, const iovec* iov, int num_iov) override {
    return connectedSend([&]() { return Api::OsSysCallsImpl::writev(fd, iov, num_iov); });
  }
  Api::SysCallSizeResult send(os_fd_t fd, void* buffer, size_t length, int flags) override {
    return connectedSend([&]() { return Api::OsSysCallsImpl::send(fd, buffer, length, flags); });
  }
  Api::SysCallIntResult connect(os_fd_t fd, const sockaddr* addr, socklen_t len) override {
    if (connect_errno_ != 0) {
      return {-1, connect_errno_};
    }
    return Api::OsSysCallsImpl::connect(fd, addr, len);
  }

  template <class F> Api::SysCallSizeResult connectedSend(F real) {
    writev_calls_++;
    if (writev_errno_ != 0) {
      const int e = writev_errno_;
      writev_errno_ = 0;
      return {-1, e};
    }
    Api::SysCallSizeResult r = real();
    if (r.return_value_ < 0 && r.errno_ == ECONNREFUSED) {
      refused_++;
    }
    return r;
  }

  std::atomic<int> writev_calls_{0};
  std::atomic<int> refused_{0};
  int writev_errno_{0};
  int connect_errno_{0};
};

class UdpStatsdConnectTest : public testing::TestWithParam<Network::Address::IpVersion> {};
INSTANTIATE_TEST_SUITE_P(IpVersions, UdpStatsdConnectTest,
                         testing::ValuesIn(TestEnvironment::getIpVersionsForTest()),
                         TestUtility::ipTestParamsToString);

TEST_P(UdpStatsdConnectTest, IpSocketIsConnected) {
  NiceMock<ThreadLocal::MockInstance> tls;
  Network::Test::UdpSyncPeer server(GetParam());
  FaultySysCalls sys_calls;
  TestThreadsafeSingletonInjector<Api::OsSysCallsImpl> injector(&sys_calls);
  UdpStatsdSink sink(tls, server.localAddress(), false);
  EXPECT_TRUE(sink.writerConnectedForTest());

  sample(sink, "test_timer", 5);
  EXPECT_EQ("envoy.test_timer:5|ms", recvString(server));
  // The connected path sends with send/writev, not sendmsg.
  EXPECT_EQ(1, sys_calls.writev_calls_.load());
  tls.shutdownThread();
}

TEST_P(UdpStatsdConnectTest, GuardOffLeavesSocketUnconnected) {
  TestScopedRuntime scoped_runtime;
  scoped_runtime.mergeValues({{std::string(ConnectGuard), "false"}});
  NiceMock<ThreadLocal::MockInstance> tls;
  Network::Test::UdpSyncPeer server(GetParam());
  UdpStatsdSink sink(tls, server.localAddress(), false);
  EXPECT_FALSE(sink.writerConnectedForTest());
  sample(sink, "test_timer", 5);
  EXPECT_EQ("envoy.test_timer:5|ms", recvString(server));
  tls.shutdownThread();
}

TEST_P(UdpStatsdConnectTest, ConnectFailureFallsBackToUnconnectedSends) {
  NiceMock<ThreadLocal::MockInstance> tls;
  Network::Test::UdpSyncPeer server(GetParam());
  FaultySysCalls sys_calls;
  sys_calls.connect_errno_ = ENETUNREACH;
  TestThreadsafeSingletonInjector<Api::OsSysCallsImpl> injector(&sys_calls);
  UdpStatsdSink sink(tls, server.localAddress(), false);
  sys_calls.connect_errno_ = 0;
  EXPECT_FALSE(sink.writerConnectedForTest());

  sample(sink, "test_timer", 5);
  EXPECT_EQ("envoy.test_timer:5|ms", recvString(server));
  EXPECT_EQ(0, sys_calls.writev_calls_.load());
  tls.shutdownThread();
}

// After an ICMP port unreachable, the next send on a connected socket fails with ECONNREFUSED and
// sends nothing. The writer sends once more, so the datagram reaches a receiver that is back.
TEST_P(UdpStatsdConnectTest, RefusedSendIsRepeated) {
  NiceMock<ThreadLocal::MockInstance> tls;
  FaultySysCalls sys_calls;
  TestThreadsafeSingletonInjector<Api::OsSysCallsImpl> injector(&sys_calls);
  Network::Address::InstanceConstSharedPtr address;
  {
    // A port with no receiver behind it.
    Network::Test::UdpSyncPeer gone(GetParam());
    address = gone.localAddress();
  }
  UdpStatsdSink sink(tls, address, false);
  ASSERT_TRUE(sink.writerConnectedForTest());
  sample(sink, "lost", 1);
  // The port unreachable comes back over loopback; give it time to be queued on the socket.
  absl::SleepFor(absl::Milliseconds(100));

  Network::SocketImpl receiver(Network::Socket::Type::Datagram, address, nullptr, {});
  RELEASE_ASSERT(receiver.setBlockingForTest(false).return_value_ != -1, "");
  ASSERT_EQ(0, receiver.bind(address).return_value_);
  sample(sink, "delivered", 2);
  EXPECT_EQ(1, sys_calls.refused_.load());
  EXPECT_EQ(3, sys_calls.writev_calls_.load());
  EXPECT_EQ("envoy.delivered:2|ms", readDatagram(receiver));
  EXPECT_TRUE(sink.writerConnectedForTest());
  tls.shutdownThread();
}

// The source address fixed by connect() disappears: the writer drops the connected socket and
// sends unconnected from then on, starting with the datagram that failed.
TEST_P(UdpStatsdConnectTest, VanishingSourceAddressFallsBackToUnconnected) {
  NiceMock<ThreadLocal::MockInstance> tls;
  Network::Test::UdpSyncPeer server(GetParam());
  FaultySysCalls sys_calls;
  TestThreadsafeSingletonInjector<Api::OsSysCallsImpl> injector(&sys_calls);
  UdpStatsdSink sink(tls, server.localAddress(), false);
  ASSERT_TRUE(sink.writerConnectedForTest());

  sys_calls.writev_errno_ = EADDRNOTAVAIL;
  sample(sink, "test_timer", 1);
  EXPECT_FALSE(sink.writerConnectedForTest());
  EXPECT_EQ("envoy.test_timer:1|ms", recvString(server));
  sample(sink, "test_timer", 2);
  EXPECT_EQ("envoy.test_timer:2|ms", recvString(server));
  EXPECT_EQ(1, sys_calls.writev_calls_.load());
  tls.shutdownThread();
}

TEST_P(UdpStatsdConnectTest, OtherSendErrorsAreNotRetried) {
  NiceMock<ThreadLocal::MockInstance> tls;
  Network::Test::UdpSyncPeer server(GetParam());
  FaultySysCalls sys_calls;
  TestThreadsafeSingletonInjector<Api::OsSysCallsImpl> injector(&sys_calls);
  UdpStatsdSink sink(tls, server.localAddress(), false);
  sys_calls.writev_errno_ = ENOBUFS;
  sample(sink, "dropped", 1);
  EXPECT_TRUE(sink.writerConnectedForTest());
  sample(sink, "test_timer", 2);
  EXPECT_EQ("envoy.test_timer:2|ms", recvString(server));
  EXPECT_EQ(2, sys_calls.writev_calls_.load());
  tls.shutdownThread();
}

#ifndef WIN32
// A Unix datagram socket stays unconnected, so it reaches a receiver that re-binds its path.
TEST(UdpStatsdConnectUdsTest, UnixDatagramSocketIsNotConnected) {
  const std::string path = TestEnvironment::unixDomainSocketPath("statsd-connect.sock");
  std::shared_ptr<Network::Address::PipeInstance> address =
      *Network::Address::PipeInstance::create(path);
  NiceMock<ThreadLocal::MockInstance> tls;
  auto bound = [&]() {
    auto sock = std::make_unique<Network::SocketImpl>(Network::Socket::Type::Datagram, address,
                                                      nullptr, Network::SocketCreationOptions{});
    RELEASE_ASSERT(sock->setBlockingForTest(false).return_value_ != -1, "");
    RELEASE_ASSERT(sock->bind(address).return_value_ == 0, "");
    return sock;
  };
  auto read = [](Network::SocketImpl& sock) { return readDatagram(sock); };
  auto receiver = bound();
  UdpStatsdSink sink(tls, address, false);
  EXPECT_FALSE(sink.writerConnectedForTest());
  sample(sink, "first", 1);
  EXPECT_EQ("envoy.first:1|ms", read(*receiver));

  // The receiver goes away and a new one binds the same path.
  receiver.reset();
  ::unlink(path.c_str());
  receiver = bound();
  sample(sink, "second", 2);
  EXPECT_EQ("envoy.second:2|ms", read(*receiver));
  tls.shutdownThread();
}
#endif

// Batching of histogram samples per thread (max_bytes_per_datagram with batching on).

class UdpStatsdSinkBatchTest : public testing::Test {
protected:
  UdpStatsdSinkBatchTest() : writer_(std::make_shared<NiceMock<MockWriter>>()) {}

  // The flush callback is created on the first sample; this hands it to the sink.
  NiceMock<Event::MockSchedulableCallback>* expectFlushCallback() {
    return new NiceMock<Event::MockSchedulableCallback>(&tls_.dispatcher_);
  }
  std::unique_ptr<UdpStatsdSink> batchingSink(uint64_t limit, bool use_tag = false,
                                              const TagFormat& format = getDefaultTagFormat()) {
    return std::make_unique<UdpStatsdSink>(tls_, writer_, use_tag, getDefaultPrefix(), limit,
                                           format, false, /*batch_histogram_samples=*/true);
  }

  NiceMock<ThreadLocal::MockInstance> tls_;
  std::shared_ptr<NiceMock<MockWriter>> writer_;
};

TEST_F(UdpStatsdSinkBatchTest, BatchLimit) {
  EXPECT_EQ(1432, batchingSink(1432)->getSampleBatchBytesForTest());
  // Batching needs a limit.
  EXPECT_EQ(0, UdpStatsdSink(tls_, writer_, false, getDefaultPrefix(), std::nullopt,
                             getDefaultTagFormat(), false, true)
                   .getSampleBatchBytesForTest());
  // Off unless asked for, even with a limit (dog_statsd and graphite_statsd).
  UdpStatsdSink flush_only(tls_, writer_, false, getDefaultPrefix(), 1432);
  EXPECT_EQ(0, flush_only.getSampleBatchBytesForTest());
  EXPECT_EQ(1432, flush_only.getBufferSizeForTest());
  // Above the largest UDP payload: lowered to it, for the flush and the samples.
  auto huge = batchingSink(1000000000);
  EXPECT_EQ(UdpStatsdSink::MAX_UDP_PAYLOAD_IPV4, huge->getSampleBatchBytesForTest());
  EXPECT_EQ(UdpStatsdSink::MAX_UDP_PAYLOAD_IPV4, huge->getBufferSizeForTest());
  tls_.shutdownThread();
}

TEST_F(UdpStatsdSinkBatchTest, SamplesOfOneTurnAreOneDatagram) {
  auto sink = batchingSink(512);
  auto* flush_cb = expectFlushCallback();

  EXPECT_CALL(*writer_, write(_)).Times(0);
  EXPECT_CALL(*flush_cb, scheduleCallbackCurrentIteration()).Times(3);
  sample(*sink, "upstream_rq_time", 5);
  EXPECT_TRUE(flush_cb->enabled_);
  sample(*sink, "external.upstream_rq_time", 5);
  sample(*sink, "downstream_rq_time", 7);
  testing::Mock::VerifyAndClearExpectations(writer_.get());
  testing::Mock::VerifyAndClearExpectations(flush_cb);

  EXPECT_CALL(*writer_, write("envoy.upstream_rq_time:5|ms\n"
                              "envoy.external.upstream_rq_time:5|ms\n"
                              "envoy.downstream_rq_time:7|ms"));
  flush_cb->invokeCallback();
  testing::Mock::VerifyAndClearExpectations(writer_.get());

  // The next turn starts a new batch with the same callback.
  EXPECT_CALL(*writer_, write("envoy.upstream_rq_time:6|ms"));
  sample(*sink, "upstream_rq_time", 6);
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
  auto sink = batchingSink(64);
  auto* flush_cb = expectFlushCallback();
  InSequence s;
  EXPECT_CALL(*writer_, write("envoy.test_timer:1|ms\nenvoy.test_timer:2|ms"));
  EXPECT_CALL(*writer_, write("envoy.test_timer:3|ms"));
  sample(*sink, "test_timer", 1);
  sample(*sink, "test_timer", 2);
  sample(*sink, "test_timer", 3);
  flush_cb->invokeCallback();
  tls_.shutdownThread();
}

TEST_F(UdpStatsdSinkBatchTest, ExactFit) {
  auto sink = batchingSink(43);
  auto* flush_cb = expectFlushCallback();
  EXPECT_CALL(*writer_, write("envoy.test_timer:1|ms\nenvoy.test_timer:2|ms"));
  sample(*sink, "test_timer", 1);
  sample(*sink, "test_timer", 2);
  flush_cb->invokeCallback();
  tls_.shutdownThread();
}

TEST_F(UdpStatsdSinkBatchTest, LineAtTheLimitIsSentAloneInOrder) {
  auto sink = batchingSink(21);
  auto* flush_cb = expectFlushCallback();
  InSequence s;
  EXPECT_CALL(*writer_, write("envoy.a:1|ms"));
  EXPECT_CALL(*writer_, write("envoy.test_timer:2|ms"));
  EXPECT_CALL(*writer_, write("envoy.a:3|ms"));
  sample(*sink, "a", 1);
  sample(*sink, "test_timer", 2);
  sample(*sink, "a", 3);
  flush_cb->invokeCallback();
  tls_.shutdownThread();
}

TEST_F(UdpStatsdSinkBatchTest, ThreadShutdownSendsPendingSamples) {
  auto sink = batchingSink(512);
  expectFlushCallback();
  sample(*sink, "test_timer", 1);
  sample(*sink, "test_timer", 2);
  EXPECT_CALL(*writer_, write("envoy.test_timer:1|ms\nenvoy.test_timer:2|ms"));
  tls_.shutdownThread();
  testing::Mock::VerifyAndClearExpectations(writer_.get());
}

TEST_F(UdpStatsdSinkBatchTest, SinkDestructionSendsPendingSamples) {
  auto sink = batchingSink(512);
  expectFlushCallback();
  sample(*sink, "test_timer", 1);
  EXPECT_CALL(*writer_, write("envoy.test_timer:1|ms"));
  sink.reset();
  testing::Mock::VerifyAndClearExpectations(writer_.get());
}

TEST_F(UdpStatsdSinkBatchTest, FlushSendsPendingSamplesFirstAndIsBatched) {
  auto sink = batchingSink(1024);
  writer_->delegateBufferFake();
  expectFlushCallback();
  NiceMock<Stats::MockMetricSnapshot> snapshot;
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

  sample(*sink, "test_timer", 1);
  EXPECT_CALL(*writer_, write("envoy.test_timer:1|ms"));
  sink->flush(snapshot);
  ASSERT_EQ(1, writer_->buffer_writes.size());
  EXPECT_EQ("envoy.test_counter:1|c\nenvoy.test_gauge:1|g", writer_->buffer_writes[0]);
  tls_.shutdownThread();
}

TEST_F(UdpStatsdSinkBatchTest, TagFormatsThroughTheBatch) {
  std::vector<Stats::Tag> tags = {Stats::Tag{"key1", "value1"}, Stats::Tag{"key2", "value2"}};
  NiceMock<Stats::MockHistogram> timer;
  timer.name_ = "test_timer";
  timer.setTags(tags);
  {
    auto sink = batchingSink(512, true);
    auto* flush_cb = expectFlushCallback();
    EXPECT_CALL(*writer_, write("envoy.test_timer:5|ms|#key1:value1,key2:value2\n"
                                "envoy.test_timer:6|ms|#key1:value1,key2:value2"));
    sink->onHistogramComplete(timer, 5);
    sink->onHistogramComplete(timer, 6);
    flush_cb->invokeCallback();
  }
  {
    auto sink = batchingSink(512, true, getGraphiteTagFormat());
    auto* flush_cb = expectFlushCallback();
    EXPECT_CALL(*writer_, write("envoy.test_timer;key1=value1;key2=value2:5|ms\n"
                                "envoy.test_timer;key1=value1;key2=value2:6|ms"));
    sink->onHistogramComplete(timer, 5);
    sink->onHistogramComplete(timer, 6);
    flush_cb->invokeCallback();
  }
  tls_.shutdownThread();
}

TEST_F(UdpStatsdSinkBatchTest, BatchingOffWritesEverySample) {
  UdpStatsdSink sink(tls_, writer_, false, getDefaultPrefix(), 512);
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
  std::unique_ptr<UdpStatsdSink> batchingSink() {
    return std::make_unique<UdpStatsdSink>(tls_, server_.localAddress(), false, getDefaultPrefix(),
                                           512, getDefaultTagFormat(), false, true);
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
  auto sink = batchingSink();
  EXPECT_TRUE(sink->writerConnectedForTest());
  sample(*sink, "upstream_rq_time", 5);
  sample(*sink, "external.upstream_rq_time", 5);
  sample(*sink, "downstream_rq_time", 7);
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);
  // The next turn's sample is the next datagram, so the first turn sent exactly one.
  sample(*sink, "upstream_rq_time", 6);
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);
  EXPECT_EQ("envoy.upstream_rq_time:5|ms\nenvoy.external.upstream_rq_time:5|ms\n"
            "envoy.downstream_rq_time:7|ms",
            recvString(server_));
  EXPECT_EQ("envoy.upstream_rq_time:6|ms", recvString(server_));
  // A pending sample is sent when the thread shuts down.
  sample(*sink, "downstream_rq_time", 8);
  tls_.shutdownThread();
  EXPECT_EQ("envoy.downstream_rq_time:8|ms", recvString(server_));
}

TEST_P(UdpStatsdSinkBatchSocketTest, LimitIsLoweredToTheLargestPayloadOfTheFamily) {
  UdpStatsdSink sink(tls_, server_.localAddress(), false, getDefaultPrefix(), 1000000000,
                     getDefaultTagFormat(), false, true);
  const uint64_t max = GetParam() == Network::Address::IpVersion::v6
                           ? UdpStatsdSink::MAX_UDP_PAYLOAD_IPV6
                           : UdpStatsdSink::MAX_UDP_PAYLOAD_IPV4;
  EXPECT_EQ(max, sink.getSampleBatchBytesForTest());
  EXPECT_EQ(max, sink.getBufferSizeForTest());
  tls_.shutdownThread();
}

TEST_P(UdpStatsdSinkBatchSocketTest, BatchingWithConnectOff) {
  TestScopedRuntime scoped_runtime;
  scoped_runtime.mergeValues({{std::string(ConnectGuard), "false"}});
  auto sink = batchingSink();
  EXPECT_FALSE(sink->writerConnectedForTest());
  sample(*sink, "a", 1);
  sample(*sink, "b", 2);
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);
  EXPECT_EQ("envoy.a:1|ms\nenvoy.b:2|ms", recvString(server_));
  tls_.shutdownThread();
}

TEST_P(UdpStatsdSinkBatchSocketTest, ConnectWithBatchingOff) {
  UdpStatsdSink sink(tls_, server_.localAddress(), false);
  EXPECT_TRUE(sink.writerConnectedForTest());
  EXPECT_EQ(0, sink.getSampleBatchBytesForTest());
  sample(sink, "a", 1);
  sample(sink, "b", 2);
  EXPECT_EQ("envoy.a:1|ms", recvString(server_));
  EXPECT_EQ("envoy.b:2|ms", recvString(server_));
  tls_.shutdownThread();
}

// The guard is read when the sink is created; flipping it later changes nothing.
TEST_P(UdpStatsdSinkBatchSocketTest, ConnectGuardIsLatched) {
  TestScopedRuntime scoped_runtime;
  auto sink = batchingSink();
  scoped_runtime.mergeValues({{std::string(ConnectGuard), "false"}});
  EXPECT_TRUE(sink->writerConnectedForTest());
  tls_.shutdownThread();
}

#ifndef WIN32
TEST(UdpStatsdSinkBatchUdsTest, BatchOverUnixDatagramSocket) {
  Api::ApiPtr api = Api::createApiForTest();
  Event::DispatcherPtr dispatcher = api->allocateDispatcher("test_thread");
  NiceMock<ThreadLocal::MockInstance> tls;
  tls.setDispatcher(dispatcher.get());
  std::shared_ptr<Network::Address::PipeInstance> address = *Network::Address::PipeInstance::create(
      TestEnvironment::unixDomainSocketPath("statsd-batch.sock"));
  Network::SocketImpl receiver(Network::Socket::Type::Datagram, address, nullptr, {});
  RELEASE_ASSERT(receiver.setBlockingForTest(false).return_value_ != -1, "");
  ASSERT_EQ(0, receiver.bind(address).return_value_);
  UdpStatsdSink sink(tls, address, false, getDefaultPrefix(), 512, getDefaultTagFormat(), false,
                     true);
  EXPECT_FALSE(sink.writerConnectedForTest());
  sample(sink, "a", 1);
  sample(sink, "b", 2);
  dispatcher->run(Event::Dispatcher::RunType::NonBlock);
  EXPECT_EQ("envoy.a:1|ms\nenvoy.b:2|ms", readDatagram(receiver));
  tls.shutdownThread();
}
#endif

// Two worker threads with the real thread-local implementation: each thread batches its own
// samples, and each sends its pending batch when it shuts down.
TEST_P(UdpStatsdSinkBatchSocketTest, PerThreadBatchesWithRealThreadLocal) {
  ThreadLocal::InstanceImpl tls;
  Event::DispatcherPtr worker_dispatchers[2] = {api_->allocateDispatcher("worker_0"),
                                                api_->allocateDispatcher("worker_1")};
  tls.registerThread(*dispatcher_, true);
  for (auto& d : worker_dispatchers) {
    tls.registerThread(*d, false);
  }
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);
  UdpStatsdSink sink(tls, server_.localAddress(), false, getDefaultPrefix(), 512,
                     getDefaultTagFormat(), false, true);

  absl::Notification ready[2];
  absl::Notification shut_down;
  std::vector<Thread::ThreadPtr> threads;
  for (int i = 0; i < 2; i++) {
    threads.push_back(Thread::threadFactoryForTest().createThread([&, i]() {
      Event::Dispatcher& d = *worker_dispatchers[i];
      d.run(Event::Dispatcher::RunType::NonBlock); // installs this thread's sink object
      const std::string w = absl::StrCat("w", i);
      sample(sink, w + ".a", 1);
      sample(sink, w + ".b", 2);
      d.run(Event::Dispatcher::RunType::NonBlock); // end of the turn: one datagram
      sample(sink, w + ".tail", 3);                // pending until the thread shuts down
      ready[i].Notify();
      shut_down.WaitForNotification();
      tls.shutdownThread();
    }));
  }
  ready[0].WaitForNotification();
  ready[1].WaitForNotification();
  tls.shutdownGlobalThreading();
  shut_down.Notify();
  for (auto& t : threads) {
    t->join();
  }
  tls.shutdownThread();

  std::set<std::string> got;
  for (int i = 0; i < 4; i++) {
    got.insert(recvString(server_));
  }
  EXPECT_EQ((std::set<std::string>{"envoy.w0.a:1|ms\nenvoy.w0.b:2|ms", "envoy.w0.tail:3|ms",
                                   "envoy.w1.a:1|ms\nenvoy.w1.b:2|ms", "envoy.w1.tail:3|ms"}),
            got);
}

} // namespace
} // namespace Statsd
} // namespace Common
} // namespace StatSinks
} // namespace Extensions
} // namespace Envoy
