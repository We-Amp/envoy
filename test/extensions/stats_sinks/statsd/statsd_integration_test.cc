#include <optional>
#include <string>
#include <tuple>

#include "envoy/config/bootstrap/v3/bootstrap.pb.h"
#include "envoy/config/metrics/v3/stats.pb.h"

#include "test/integration/http_integration.h"
#include "test/test_common/network_utility.h"

#include "absl/strings/match.h"
#include "gtest/gtest.h"

namespace Envoy {
namespace {

// A real server with the UDP statsd sink: count what one proxied request sends to the receiver.
class StatsdSinkIntegrationTest : public testing::TestWithParam<Network::Address::IpVersion>,
                                  public HttpIntegrationTest {
public:
  StatsdSinkIntegrationTest() : HttpIntegrationTest(Http::CodecType::HTTP1, GetParam()) {}

  void initializeWithSink(std::optional<uint64_t> max_bytes_per_datagram) {
    statsd_ = std::make_unique<Network::Test::UdpSyncPeer>(version_);
    const Network::Address::InstanceConstSharedPtr address = statsd_->localAddress();
    config_helper_.addConfigModifier(
        [address, max_bytes_per_datagram](envoy::config::bootstrap::v3::Bootstrap& bootstrap) {
          envoy::config::metrics::v3::StatsdSink config;
          auto* socket_address = config.mutable_address()->mutable_socket_address();
          socket_address->set_protocol(envoy::config::core::v3::SocketAddress::UDP);
          socket_address->set_address(address->ip()->addressAsString());
          socket_address->set_port_value(address->ip()->port());
          if (max_bytes_per_datagram.has_value()) {
            config.mutable_max_bytes_per_datagram()->set_value(*max_bytes_per_datagram);
          }
          auto* sink = bootstrap.add_stats_sinks();
          sink->set_name("envoy.stat_sinks.statsd");
          std::ignore = sink->mutable_typed_config()->PackFrom(config);
          // No stats flush during the test: only histogram samples reach the receiver.
          bootstrap.mutable_stats_flush_interval()->set_seconds(60);
        });
    initialize();
  }

  // The first datagram that contains `needle`, skipping the samples recorded at startup.
  std::string datagramWith(absl::string_view needle) {
    while (true) {
      Network::UdpRecvData data;
      statsd_->recv(data);
      std::string payload = data.buffer_->toString();
      if (absl::StrContains(payload, needle)) {
        return payload;
      }
    }
  }

  void oneRequest() {
    codec_client_ = makeHttpConnection(lookupPort("http"));
    auto response =
        sendRequestAndWaitForResponse(default_request_headers_, 0, default_response_headers_, 0);
    EXPECT_TRUE(response->complete());
    codec_client_->close();
  }

  std::unique_ptr<Network::Test::UdpSyncPeer> statsd_;
};

INSTANTIATE_TEST_SUITE_P(IpVersions, StatsdSinkIntegrationTest,
                         testing::ValuesIn(TestEnvironment::getIpVersionsForTest()),
                         TestUtility::ipTestParamsToString);

// With max_bytes_per_datagram, the samples the worker records for one request (the router's
// upstream_rq_time and the connection manager's downstream_rq_time, in one loop iteration)
// arrive as one datagram.
TEST_P(StatsdSinkIntegrationTest, SamplesOfOneRequestAreOneDatagram) {
  initializeWithSink(1432);
  oneRequest();
  const std::string datagram = datagramWith("cluster.cluster_0.upstream_rq_time:");
  EXPECT_TRUE(absl::StrContains(datagram, "downstream_rq_time:")) << datagram;
  EXPECT_TRUE(absl::StrContains(datagram, "\n")) << datagram;
}

// Without it, every sample is a datagram of its own, as before.
TEST_P(StatsdSinkIntegrationTest, DefaultIsOneMetricPerDatagram) {
  initializeWithSink(std::nullopt);
  oneRequest();
  const std::string datagram = datagramWith("cluster.cluster_0.upstream_rq_time:");
  EXPECT_FALSE(absl::StrContains(datagram, "\n")) << datagram;
  EXPECT_FALSE(absl::StrContains(datagram, "downstream_rq_time:")) << datagram;
  const std::string next = datagramWith("downstream_rq_time:");
  EXPECT_FALSE(absl::StrContains(next, "\n")) << next;
}

} // namespace
} // namespace Envoy
