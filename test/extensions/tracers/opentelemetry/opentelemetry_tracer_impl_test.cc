#include <sys/types.h>

#include "envoy/common/exception.h"
#include "envoy/registry/registry.h"

#include "source/common/tracing/http_tracer_impl.h"
#include "source/common/tracing/tracer_impl.h"
#include "source/common/version/version.h"
#include "source/extensions/tracers/opentelemetry/opentelemetry_tracer_impl.h"
#include "source/extensions/tracers/opentelemetry/span_context_extractor.h"
#include "source/extensions/tracers/opentelemetry/trace_exporter.h"

#include "test/mocks/common.h"
#include "test/mocks/event/mocks.h"
#include "test/mocks/local_info/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/mocks/server/tracer_factory_context.h"
#include "test/mocks/stats/mocks.h"
#include "test/mocks/stream_info/mocks.h"
#include "test/mocks/tracing/mocks.h"
#include "test/test_common/test_runtime.h"
#include "test/test_common/utility.h"

#include "absl/status/statusor.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace Tracers {
namespace OpenTelemetry {

using testing::_;
using testing::InSequence;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;
using testing::ReturnRef;

class MockResourceProvider : public ResourceProvider {
public:
  MOCK_METHOD(Resource, getResource,
              (const Protobuf::RepeatedPtrField<envoy::config::core::v3::TypedExtensionConfig>&
                   resource_detectors,
               Server::Configuration::ServerFactoryContext& context, absl::string_view service_name,
               const ResourceProviderOptions& options),
              (const));
};

namespace {

class DummyTraceExporter : public OpenTelemetryTraceExporter {
public:
  bool log(const ExportTraceServiceRequest& /*request*/) override {
    logged_count_++;
    return true;
  }

  inline static std::atomic<uint32_t> logged_count_{0};
};

class DummyTraceExporterFactory : public OpenTelemetryTraceExporterFactory {
public:
  absl::StatusOr<OpenTelemetryTraceExporterPtr>
  createExporter(const Protobuf::Message& config,
                 Server::Configuration::TracerFactoryContext& /*context*/) const override {
    EXPECT_NE(Envoy::Protobuf::DynamicCastMessage<Protobuf::Empty>(&config), nullptr);
    return std::make_unique<DummyTraceExporter>();
  }

  ProtobufTypes::MessagePtr createEmptyConfigProto() override {
    return std::make_unique<Protobuf::Empty>();
  }

  std::string name() const override {
    return "envoy.tracers.opentelemetry.exporters.dummy_tracer_impl_test";
  }
};

REGISTER_FACTORY(DummyTraceExporterFactory, OpenTelemetryTraceExporterFactory);

class NullTraceExporterFactory : public OpenTelemetryTraceExporterFactory {
public:
  absl::StatusOr<OpenTelemetryTraceExporterPtr>
  createExporter(const Protobuf::Message& /*config*/,
                 Server::Configuration::TracerFactoryContext& /*context*/) const override {
    return OpenTelemetryTraceExporterPtr(nullptr);
  }

  ProtobufTypes::MessagePtr createEmptyConfigProto() override {
    return std::make_unique<Protobuf::Duration>();
  }

  std::string name() const override {
    return "envoy.tracers.opentelemetry.exporters.null_tracer_test";
  }
};

REGISTER_FACTORY(NullTraceExporterFactory, OpenTelemetryTraceExporterFactory);

} // namespace

class OpenTelemetryDriverTest : public testing::Test {
public:
  OpenTelemetryDriverTest() = default;

  void setup(envoy::config::trace::v3::OpenTelemetryConfig& opentelemetry_config) {
    auto mock_client_factory = std::make_unique<NiceMock<Grpc::MockAsyncClientFactory>>();
    auto mock_client = std::make_unique<NiceMock<Grpc::MockAsyncClient>>();
    mock_client_ = mock_client.get();
    ON_CALL(*mock_client_factory, createUncachedRawAsyncClient())
        .WillByDefault(Return(ByMove(std::move(mock_client))));
    auto& factory_context = context_.server_factory_context_;
    ON_CALL(factory_context, runtime()).WillByDefault(ReturnRef(runtime_));
    ON_CALL(factory_context.cluster_manager_.async_client_manager_, factoryForGrpcService(_, _, _))
        .WillByDefault(Return(ByMove(std::move(mock_client_factory))));
    ON_CALL(factory_context, scope()).WillByDefault(ReturnRef(scope_));

    Resource resource;
    resource.attributes_.insert(std::pair<std::string, std::string>("key1", "val1"));

    auto mock_resource_provider = NiceMock<MockResourceProvider>();
    EXPECT_CALL(mock_resource_provider, getResource(_, _, _, _)).WillRepeatedly(Return(resource));

    driver_ = std::make_unique<Driver>(opentelemetry_config, context_, mock_resource_provider);
  }

  void setupValidDriver() {
    const std::string yaml_string = R"EOF(
    grpc_service:
      envoy_grpc:
        cluster_name: fake-cluster
      timeout: 0.250s
    )EOF";
    envoy::config::trace::v3::OpenTelemetryConfig opentelemetry_config;
    TestUtility::loadFromYaml(yaml_string, opentelemetry_config);

    setup(opentelemetry_config);
  }

  void setupValidDriverWithHttpExporter() {
    const std::string yaml_string = R"EOF(
    http_service:
      http_uri:
        cluster: "my_o11y_backend"
        uri: "https://some-o11y.com/otlp/v1/traces"
        timeout: 0.250s
      request_headers_to_add:
      - header:
          key: "Authorization"
          value: "auth-token"
    )EOF";
    envoy::config::trace::v3::OpenTelemetryConfig opentelemetry_config;
    TestUtility::loadFromYaml(yaml_string, opentelemetry_config);

    setup(opentelemetry_config);
  }

  void setupValidDriverWithAlwaysOnSampler() {
    const std::string yaml_string = R"EOF(
    grpc_service:
      envoy_grpc:
        cluster_name: fake-cluster
      timeout: 0.250s
    sampler:
      name: envoy.tracers.opentelemetry.samplers.always_on
      typed_config:
        "@type": type.googleapis.com/envoy.extensions.tracers.opentelemetry.samplers.v3.AlwaysOnSamplerConfig
    )EOF";
    envoy::config::trace::v3::OpenTelemetryConfig opentelemetry_config;
    TestUtility::loadFromYaml(yaml_string, opentelemetry_config);

    setup(opentelemetry_config);
  }

  void setupValidDriverWithCustomExporter() {
    const std::string yaml_string = R"EOF(
    exporter:
      name: envoy.tracers.opentelemetry.exporters.dummy_tracer_impl_test
      typed_config:
        "@type": type.googleapis.com/google.protobuf.Empty
    )EOF";
    envoy::config::trace::v3::OpenTelemetryConfig opentelemetry_config;
    TestUtility::loadFromYaml(yaml_string, opentelemetry_config);

    setup(opentelemetry_config);
  }

protected:
  const std::string operation_name_{"test"};
  NiceMock<Envoy::Server::Configuration::MockTracerFactoryContext> context_;
  NiceMock<Envoy::Tracing::MockConfig> mock_tracing_config_;
  NiceMock<StreamInfo::MockStreamInfo> stream_info_;
  Event::SimulatedTimeSystem time_system_;
  NiceMock<Grpc::MockAsyncClient>* mock_client_{nullptr};
  envoy::config::trace::v3::OpenTelemetryConfig config_;
  Tracing::DriverPtr driver_;
  NiceMock<Runtime::MockLoader> runtime_;
  NiceMock<Event::MockTimer>* timer_;
  NiceMock<Stats::MockIsolatedStatsStore> stats_;
  Stats::Scope& scope_{*stats_.rootScope()};
};

// Tests the tracer initialization with the gRPC exporter
TEST_F(OpenTelemetryDriverTest, InitializeDriverValidConfig) {
  setupValidDriver();
  EXPECT_NE(driver_, nullptr);
}

// Tests the tracer initialization with the custom exporter
TEST_F(OpenTelemetryDriverTest, InitializeDriverValidConfigCustomExporter) {
  setupValidDriverWithCustomExporter();
  EXPECT_NE(driver_, nullptr);
}

// Tests exporting a span with custom exporter
TEST_F(OpenTelemetryDriverTest, ExportOTLPSpanCustomExporter) {
  DummyTraceExporter::logged_count_ = 0;
  setupValidDriverWithCustomExporter();

  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};

  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U))
      .WillRepeatedly(Return(1));

  Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                             operation_name_, {Tracing::Reason::Sampling, true});
  EXPECT_NE(span, nullptr);

  span->finishSpan();

  EXPECT_EQ(1U, DummyTraceExporter::logged_count_);
  EXPECT_EQ(1U, stats_.counter("tracing.opentelemetry.spans_sent").value());
}

// Tests the tracer initialization with the HTTP exporter
TEST_F(OpenTelemetryDriverTest, InitializeDriverValidConfigHttpExporter) {
  setupValidDriverWithHttpExporter();
  EXPECT_NE(driver_, nullptr);
}

// Verifies that set_telemetry_sdk_resource_attributes=false is passed to ResourceProvider
TEST_F(OpenTelemetryDriverTest, PassSetTelemetrySdkResourceAttributesFalse) {
  const std::string yaml_string = R"EOF(
    grpc_service:
      envoy_grpc:
        cluster_name: fake-cluster
      timeout: 0.250s
    set_telemetry_sdk_resource_attributes: false
    )EOF";
  envoy::config::trace::v3::OpenTelemetryConfig opentelemetry_config;
  TestUtility::loadFromYaml(yaml_string, opentelemetry_config);

  auto mock_client_factory = std::make_unique<NiceMock<Grpc::MockAsyncClientFactory>>();
  auto mock_client = std::make_unique<NiceMock<Grpc::MockAsyncClient>>();
  mock_client_ = mock_client.get();
  ON_CALL(*mock_client_factory, createUncachedRawAsyncClient())
      .WillByDefault(Return(ByMove(std::move(mock_client))));
  auto& factory_context = context_.server_factory_context_;
  ON_CALL(factory_context, runtime()).WillByDefault(ReturnRef(runtime_));
  ON_CALL(factory_context.cluster_manager_.async_client_manager_, factoryForGrpcService(_, _, _))
      .WillByDefault(Return(ByMove(std::move(mock_client_factory))));
  ON_CALL(factory_context, scope()).WillByDefault(ReturnRef(scope_));

  Resource resource;
  auto mock_resource_provider = NiceMock<MockResourceProvider>();

  ResourceProviderOptions expected_options;
  expected_options.set_telemetry_sdk_resource_attributes = false;
  EXPECT_CALL(mock_resource_provider, getResource(_, _, _, expected_options))
      .WillOnce(Return(resource));

  driver_ = std::make_unique<Driver>(opentelemetry_config, context_, mock_resource_provider);
}

// Verifies that set_telemetry_sdk_resource_attributes defaults to true
TEST_F(OpenTelemetryDriverTest, PassSetTelemetrySdkResourceAttributesDefaultTrue) {
  const std::string yaml_string = R"EOF(
    grpc_service:
      envoy_grpc:
        cluster_name: fake-cluster
      timeout: 0.250s
    )EOF";
  envoy::config::trace::v3::OpenTelemetryConfig opentelemetry_config;
  TestUtility::loadFromYaml(yaml_string, opentelemetry_config);

  auto mock_client_factory = std::make_unique<NiceMock<Grpc::MockAsyncClientFactory>>();
  auto mock_client = std::make_unique<NiceMock<Grpc::MockAsyncClient>>();
  mock_client_ = mock_client.get();
  ON_CALL(*mock_client_factory, createUncachedRawAsyncClient())
      .WillByDefault(Return(ByMove(std::move(mock_client))));
  auto& factory_context = context_.server_factory_context_;
  ON_CALL(factory_context, runtime()).WillByDefault(ReturnRef(runtime_));
  ON_CALL(factory_context.cluster_manager_.async_client_manager_, factoryForGrpcService(_, _, _))
      .WillByDefault(Return(ByMove(std::move(mock_client_factory))));
  ON_CALL(factory_context, scope()).WillByDefault(ReturnRef(scope_));

  Resource resource;
  auto mock_resource_provider = NiceMock<MockResourceProvider>();

  ResourceProviderOptions expected_options;
  EXPECT_CALL(mock_resource_provider, getResource(_, _, _, expected_options))
      .WillOnce(Return(resource));

  driver_ = std::make_unique<Driver>(opentelemetry_config, context_, mock_resource_provider);
}

// Verifies that set_service_name_resource_attribute=false is passed to ResourceProvider
TEST_F(OpenTelemetryDriverTest, PassSetServiceNameResourceAttributeFalse) {
  const std::string yaml_string = R"EOF(
    grpc_service:
      envoy_grpc:
        cluster_name: fake-cluster
      timeout: 0.250s
    set_service_name_resource_attribute: false
    )EOF";
  envoy::config::trace::v3::OpenTelemetryConfig opentelemetry_config;
  TestUtility::loadFromYaml(yaml_string, opentelemetry_config);

  auto mock_client_factory = std::make_unique<NiceMock<Grpc::MockAsyncClientFactory>>();
  auto mock_client = std::make_unique<NiceMock<Grpc::MockAsyncClient>>();
  mock_client_ = mock_client.get();
  ON_CALL(*mock_client_factory, createUncachedRawAsyncClient())
      .WillByDefault(Return(ByMove(std::move(mock_client))));
  auto& factory_context = context_.server_factory_context_;
  ON_CALL(factory_context, runtime()).WillByDefault(ReturnRef(runtime_));
  ON_CALL(factory_context.cluster_manager_.async_client_manager_, factoryForGrpcService(_, _, _))
      .WillByDefault(Return(ByMove(std::move(mock_client_factory))));
  ON_CALL(factory_context, scope()).WillByDefault(ReturnRef(scope_));

  Resource resource;
  auto mock_resource_provider = NiceMock<MockResourceProvider>();

  ResourceProviderOptions expected_options;
  expected_options.set_service_name_resource_attribute = false;
  EXPECT_CALL(mock_resource_provider, getResource(_, _, _, expected_options))
      .WillOnce(Return(resource));

  driver_ = std::make_unique<Driver>(opentelemetry_config, context_, mock_resource_provider);
}

// Verifies that set_service_name_resource_attribute defaults to true
TEST_F(OpenTelemetryDriverTest, PassSetServiceNameResourceAttributeDefaultTrue) {
  const std::string yaml_string = R"EOF(
    grpc_service:
      envoy_grpc:
        cluster_name: fake-cluster
      timeout: 0.250s
    )EOF";
  envoy::config::trace::v3::OpenTelemetryConfig opentelemetry_config;
  TestUtility::loadFromYaml(yaml_string, opentelemetry_config);

  auto mock_client_factory = std::make_unique<NiceMock<Grpc::MockAsyncClientFactory>>();
  auto mock_client = std::make_unique<NiceMock<Grpc::MockAsyncClient>>();
  mock_client_ = mock_client.get();
  ON_CALL(*mock_client_factory, createUncachedRawAsyncClient())
      .WillByDefault(Return(ByMove(std::move(mock_client))));
  auto& factory_context = context_.server_factory_context_;
  ON_CALL(factory_context, runtime()).WillByDefault(ReturnRef(runtime_));
  ON_CALL(factory_context.cluster_manager_.async_client_manager_, factoryForGrpcService(_, _, _))
      .WillByDefault(Return(ByMove(std::move(mock_client_factory))));
  ON_CALL(factory_context, scope()).WillByDefault(ReturnRef(scope_));

  Resource resource;
  auto mock_resource_provider = NiceMock<MockResourceProvider>();

  ResourceProviderOptions expected_options;
  EXPECT_CALL(mock_resource_provider, getResource(_, _, _, expected_options))
      .WillOnce(Return(resource));

  driver_ = std::make_unique<Driver>(opentelemetry_config, context_, mock_resource_provider);
}

// Verifies that set_instrumentation_scope=false omits scope name and version
TEST_F(OpenTelemetryDriverTest, SetInstrumentationScopeFalse) {
  const std::string yaml_string = R"EOF(
    grpc_service:
      envoy_grpc:
        cluster_name: fake-cluster
      timeout: 0.250s
    set_instrumentation_scope: false
    )EOF";
  envoy::config::trace::v3::OpenTelemetryConfig opentelemetry_config;
  TestUtility::loadFromYaml(yaml_string, opentelemetry_config);

  setup(opentelemetry_config);

  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};

  Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                             "test", {Tracing::Reason::Sampling, true});
  EXPECT_NE(span, nullptr);

  constexpr absl::string_view request_yaml = R"(
resource_spans:
  resource:
    attributes:
      key: "service.name"
      value:
        string_value: "unknown_service:envoy"
      key: "key1"
      value:
        string_value: "val1"
  scope_spans:
    scope: {{}}
    spans:
      name: "test"
      kind: SPAN_KIND_SERVER
      start_time_unix_nano: {}
      end_time_unix_nano: {}
      flags: 257
  )";
  opentelemetry::proto::collector::trace::v1::ExportTraceServiceRequest request_proto;
  SystemTime timestamp = time_system_.systemTime();
  ON_CALL(stream_info_, startTime()).WillByDefault(Return(timestamp));

  int64_t timestamp_ns = std::chrono::nanoseconds(timestamp.time_since_epoch()).count();

  TestUtility::loadFromYaml(fmt::format(request_yaml, timestamp_ns, timestamp_ns), request_proto);
  auto* expected_span =
      request_proto.mutable_resource_spans(0)->mutable_scope_spans(0)->mutable_spans(0);
  expected_span->set_trace_id(absl::HexStringToBytes(span->getTraceId()));
  expected_span->set_span_id(absl::HexStringToBytes(span->getSpanId()));

  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U))
      .WillRepeatedly(Return(1));
  EXPECT_CALL(
      *mock_client_,
      sendRaw(_, _, Grpc::ProtoBufferEqIgnoreRepeatedFieldOrdering(request_proto), _, _, _));
  span->finishSpan();
  EXPECT_EQ(1U, stats_.counter("tracing.opentelemetry.spans_sent").value());
}

// Verifies that when a gRPC async client fails to initialize, spans are dropped
// gracefully and spans_dropped metric is incremented.
TEST_F(OpenTelemetryDriverTest, UnconfiguredExporterIncrementsSpansDropped) {
  const std::string yaml_string = R"EOF(
    grpc_service:
      envoy_grpc:
        cluster_name: fake-cluster
      timeout: 0.250s
    )EOF";
  envoy::config::trace::v3::OpenTelemetryConfig opentelemetry_config;
  TestUtility::loadFromYaml(yaml_string, opentelemetry_config);

  auto mock_client_factory = std::make_unique<NiceMock<Grpc::MockAsyncClientFactory>>();
  EXPECT_CALL(*mock_client_factory, createUncachedRawAsyncClient())
      .WillOnce(Return(absl::InternalError("gRPC client creation failed")));

  auto& factory_context = context_.server_factory_context_;
  ON_CALL(factory_context, runtime()).WillByDefault(ReturnRef(runtime_));
  ON_CALL(factory_context.cluster_manager_.async_client_manager_, factoryForGrpcService(_, _, _))
      .WillByDefault(Return(ByMove(std::move(mock_client_factory))));
  ON_CALL(factory_context, scope()).WillByDefault(ReturnRef(scope_));

  Resource resource;
  auto mock_resource_provider = NiceMock<MockResourceProvider>();
  EXPECT_CALL(mock_resource_provider, getResource(_, _, _, _)).WillRepeatedly(Return(resource));

  driver_ = std::make_unique<Driver>(opentelemetry_config, context_, mock_resource_provider);

  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};
  SystemTime timestamp = time_system_.systemTime();
  ON_CALL(stream_info_, startTime()).WillByDefault(Return(timestamp));

  Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                             operation_name_, {Tracing::Reason::Sampling, true});
  EXPECT_NE(span.get(), nullptr);

  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U))
      .WillRepeatedly(Return(1));

  span->finishSpan();

  EXPECT_EQ(1U, stats_.counter("tracing.opentelemetry.spans_dropped").value());
  EXPECT_EQ(0U, stats_.counter("tracing.opentelemetry.spans_sent").value());
}

// Verifies that when custom exporter factory returns nullptr, spans are dropped
// gracefully and spans_dropped metric is incremented.
TEST_F(OpenTelemetryDriverTest, NullCustomExporterIncrementsSpansDropped) {
  const std::string yaml_string = R"EOF(
    exporter:
      name: envoy.tracers.opentelemetry.exporters.null_tracer_test
      typed_config:
        "@type": type.googleapis.com/google.protobuf.Duration
    )EOF";
  envoy::config::trace::v3::OpenTelemetryConfig opentelemetry_config;
  TestUtility::loadFromYaml(yaml_string, opentelemetry_config);

  setup(opentelemetry_config);

  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};
  SystemTime timestamp = time_system_.systemTime();
  ON_CALL(stream_info_, startTime()).WillByDefault(Return(timestamp));

  Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                             operation_name_, {Tracing::Reason::Sampling, true});
  EXPECT_NE(span.get(), nullptr);

  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U))
      .WillRepeatedly(Return(1));

  span->finishSpan();

  EXPECT_EQ(1U, stats_.counter("tracing.opentelemetry.spans_dropped").value());
  EXPECT_EQ(0U, stats_.counter("tracing.opentelemetry.spans_sent").value());
}

// Verifies that the tracer cannot be configured with multiple exporters at the same time
TEST_F(OpenTelemetryDriverTest, MultipleExportersConfigured) {
  const std::string yaml_string = R"EOF(
    grpc_service:
      envoy_grpc:
        cluster_name: fake-cluster
      timeout: 0.250s
    http_service:
      http_uri:
        cluster: "my_o11y_backend"
        uri: "https://some-o11y.com/otlp/v1/traces"
        timeout: 0.250s
    exporter:
      name: envoy.tracers.opentelemetry.exporters.dummy_tracer_impl_test
      typed_config:
        "@type": type.googleapis.com/google.protobuf.Empty
    )EOF";
  envoy::config::trace::v3::OpenTelemetryConfig opentelemetry_config;
  TestUtility::loadFromYaml(yaml_string, opentelemetry_config);
  EXPECT_THROW_WITH_MESSAGE(
      setup(opentelemetry_config), EnvoyException,
      "OpenTelemetry Tracer must have exactly one of gRPC, HTTP, or custom exporter configured.");
  EXPECT_EQ(driver_, nullptr);
}

// Verifies that an exception is thrown when an unknown custom exporter factory
// is configured
TEST_F(OpenTelemetryDriverTest, CustomExporterFactoryNotFound) {
  const std::string yaml_string = R"EOF(
    exporter:
      name: envoy.tracers.opentelemetry.exporters.unknown
      typed_config:
        "@type": type.googleapis.com/google.protobuf.Timestamp
    )EOF";
  envoy::config::trace::v3::OpenTelemetryConfig opentelemetry_config;
  TestUtility::loadFromYaml(yaml_string, opentelemetry_config);

  EXPECT_THROW_WITH_MESSAGE(setup(opentelemetry_config), EnvoyException,
                            "OpenTelemetry trace exporter factory not found: "
                            "'envoy.tracers.opentelemetry.exporters.unknown'");
  EXPECT_EQ(driver_, nullptr);
}

// Verifies that an exception is thrown when an invalid opaque configuration is
// provided to a custom exporter factory.
TEST_F(OpenTelemetryDriverTest, CustomExporterInvalidOpaqueConfig) {
  envoy::config::trace::v3::OpenTelemetryConfig opentelemetry_config;
  auto* exporter = opentelemetry_config.mutable_exporter();
  exporter->set_name("envoy.tracers.opentelemetry.exporters.dummy_tracer_impl_test");
  exporter->mutable_typed_config()->set_type_url("type.googleapis.com/google.protobuf.Empty");
  exporter->mutable_typed_config()->set_value("invalid_protobuf_data_\xff\xff");

  EXPECT_THROW_WITH_REGEX(setup(opentelemetry_config), EnvoyException,
                          ".*Failed to translate opaque config for "
                          "OpenTelemetry trace exporter factory "
                          "'envoy.tracers.opentelemetry.exporters.dummy_tracer_impl_test'.*");
  EXPECT_EQ(driver_, nullptr);
}

// Verifies traceparent/tracestate headers are properly parsed and propagated
TEST_F(OpenTelemetryDriverTest, ParseSpanContextFromHeadersTest) {
  // Set up driver
  setupValidDriver();

  // Add the OTLP headers to the request headers
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};
  // traceparent header is "version-trace_id-parent_id-trace_flags"
  // See https://w3c.github.io/trace-context/#traceparent-header
  const std::string version = "00";
  const uint64_t trace_id_high = 0;
  const uint64_t trace_id_low = 1;
  const std::string trace_id_hex =
      absl::StrCat(Hex::uint64ToHex(trace_id_high), Hex::uint64ToHex(trace_id_low));
  const uint64_t parent_span_id = 2;
  const std::string trace_flags = "01";
  const std::vector<std::string> v = {version, trace_id_hex, Hex::uint64ToHex(parent_span_id),
                                      trace_flags};
  const std::string parent_trace_header = absl::StrJoin(v, "-");
  request_headers.set(OpenTelemetryConstants::get().TRACE_PARENT.key(), parent_trace_header);
  // Also add tracestate.
  request_headers.set(OpenTelemetryConstants::get().TRACE_STATE.key(), "test=foo");

  // Mock the random call for generating span ID so we can check it later.
  const uint64_t new_span_id = 3;
  NiceMock<Random::MockRandomGenerator>& mock_random_generator_ =
      context_.server_factory_context_.api_.random_;
  ON_CALL(mock_random_generator_, random()).WillByDefault(Return(new_span_id));

  Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                             operation_name_, {Tracing::Reason::Sampling, true});

  EXPECT_EQ(span->getTraceId(), trace_id_hex);

  // Remove headers, then inject context into header from the span.
  request_headers.remove(OpenTelemetryConstants::get().TRACE_PARENT.key());
  request_headers.remove(OpenTelemetryConstants::get().TRACE_STATE.key());
  span->injectContext(request_headers, Tracing::UpstreamContext());

  auto sampled_entry = request_headers.get(OpenTelemetryConstants::get().TRACE_PARENT.key());
  EXPECT_EQ(sampled_entry.has_value(), true);
  EXPECT_EQ(
      sampled_entry.value(),
      absl::StrJoin({version, trace_id_hex, Hex::uint64ToHex(new_span_id), trace_flags}, "-"));

  auto sampled_tracestate_entry =
      request_headers.get(OpenTelemetryConstants::get().TRACE_STATE.key());
  EXPECT_EQ(sampled_tracestate_entry.has_value(), true);
  EXPECT_EQ(sampled_tracestate_entry.value(), "test=foo");
  constexpr absl::string_view request_yaml = R"(
resource_spans:
  resource:
    attributes:
      key: "service.name"
      value:
        string_value: "unknown_service:envoy"
      key: "key1"
      value:
        string_value: "val1"
  scope_spans:
    scope:
      name: "envoy"
      version: {}
    spans:
      trace_id: "AAA"
      span_id: "AAA"
      name: "test"
      kind: SPAN_KIND_SERVER
      start_time_unix_nano: {}
      end_time_unix_nano: {}
      trace_state: "test=foo"
      flags: 769
  )";
  opentelemetry::proto::collector::trace::v1::ExportTraceServiceRequest request_proto;
  SystemTime timestamp = time_system_.systemTime();
  ON_CALL(stream_info_, startTime()).WillByDefault(Return(timestamp));

  int64_t timestamp_ns = std::chrono::nanoseconds(timestamp.time_since_epoch()).count();
  absl::string_view envoy_version = Envoy::VersionInfo::version();

  TestUtility::loadFromYaml(fmt::format(request_yaml, envoy_version, timestamp_ns, timestamp_ns),
                            request_proto);
  auto* expected_span =
      request_proto.mutable_resource_spans(0)->mutable_scope_spans(0)->mutable_spans(0);
  expected_span->set_trace_id(absl::HexStringToBytes(trace_id_hex));
  expected_span->set_span_id(absl::HexStringToBytes(absl::StrCat(Hex::uint64ToHex(new_span_id))));
  expected_span->set_parent_span_id(
      absl::HexStringToBytes(absl::StrCat(Hex::uint64ToHex(parent_span_id))));

  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U))
      .Times(1)
      .WillRepeatedly(Return(1));
  EXPECT_CALL(
      *mock_client_,
      sendRaw(_, _, Grpc::ProtoBufferEqIgnoreRepeatedFieldOrdering(request_proto), _, _, _));
  span->finishSpan();
  EXPECT_EQ(1U, stats_.counter("tracing.opentelemetry.spans_sent").value());
}

// Verifies span is properly created when the incoming request has no traceparent/tracestate headers
TEST_F(OpenTelemetryDriverTest, GenerateSpanContextWithoutHeadersTest) {
  // Set up driver
  setupValidDriver();

  // Add the OTLP headers to the request headers
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};

  // Mock the random call for generating trace and span IDs so we can check it later.
  const uint64_t trace_id_high = 1;
  const uint64_t trace_id_low = 2;
  const uint64_t new_span_id = 3;
  NiceMock<Random::MockRandomGenerator>& mock_random_generator_ =
      context_.server_factory_context_.api_.random_;
  // The tracer should generate three random numbers for the trace high, trace low, and span id.
  {
    InSequence s;

    EXPECT_CALL(mock_random_generator_, random()).WillOnce(Return(trace_id_high));
    EXPECT_CALL(mock_random_generator_, random()).WillOnce(Return(trace_id_low));
    EXPECT_CALL(mock_random_generator_, random()).WillOnce(Return(new_span_id));
  }

  Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                             operation_name_, {Tracing::Reason::Sampling, true});

  // Remove headers, then inject context into header from the span.
  request_headers.remove(OpenTelemetryConstants::get().TRACE_PARENT.key());
  span->injectContext(request_headers, Tracing::UpstreamContext());

  auto sampled_entry = request_headers.get(OpenTelemetryConstants::get().TRACE_PARENT.key());

  // Ends in 01 because span should be sampled. See
  // https://w3c.github.io/trace-context/#trace-flags.
  EXPECT_EQ(sampled_entry.has_value(), true);
  EXPECT_EQ(sampled_entry.value(), "00-00000000000000010000000000000002-0000000000000003-01");
}

// Verifies a span it not created when an invalid traceparent header is received
TEST_F(OpenTelemetryDriverTest, NullSpanWithPropagationHeaderError) {
  setupValidDriver();
  // Add an invalid OTLP header to the request headers.
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};
  request_headers.set(OpenTelemetryConstants::get().TRACE_PARENT.key(),
                      "invalid00-0000000000000003-01");

  Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                             operation_name_, {Tracing::Reason::Sampling, true});

  auto& null_span = *span;
  EXPECT_EQ(typeid(null_span).name(), typeid(Tracing::NullSpan).name());
}

// Verifies the export happens after one span is created
TEST_F(OpenTelemetryDriverTest, ExportOTLPSpan) {
  // Set up driver
  setupValidDriver();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};

  Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                             operation_name_, {Tracing::Reason::Sampling, true});
  EXPECT_NE(span.get(), nullptr);

  // Test baggage noop and other noop calls.
  span->setBaggage("baggage_key", "baggage_value");
  EXPECT_TRUE(span->getBaggage("baggage_key").empty());
  span->setOperation("operation");
  span->setTag("tag_name", "tag_value");
  span->log(time_system_.systemTime(), "event");

  // Flush after a single span.
  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U))
      .Times(1)
      .WillRepeatedly(Return(1));
  // We should see a call to sendMessage to export that single span.
  EXPECT_CALL(*mock_client_, sendRaw(_, _, _, _, _, _));
  span->finishSpan();
  EXPECT_EQ(1U, stats_.counter("tracing.opentelemetry.spans_sent").value());
}

// Verifies the export happens only when a second span is created
TEST_F(OpenTelemetryDriverTest, ExportOTLPSpanWithBuffer) {
  // Set up driver
  setupValidDriver();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};

  Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                             operation_name_, {Tracing::Reason::Sampling, true});
  EXPECT_NE(span.get(), nullptr);

  // Flush after two spans.
  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U))
      .Times(2)
      .WillRepeatedly(Return(2));
  // We should not yet see a call to sendMessage to export that single span.
  span->finishSpan();
  // Once we create a
  Tracing::SpanPtr second_span =
      driver_->startSpan(mock_tracing_config_, request_headers, stream_info_, operation_name_,
                         {Tracing::Reason::Sampling, true});
  EXPECT_NE(second_span.get(), nullptr);
  // Only now should we see the span exported.
  EXPECT_CALL(*mock_client_, sendRaw(_, _, _, _, _, _));
  second_span->finishSpan();
  EXPECT_EQ(2U, stats_.counter("tracing.opentelemetry.spans_sent").value());
}

// Verifies that spans beyond max_cache_size are discarded
TEST_F(OpenTelemetryDriverTest, ExportOTLPSpanWithMaxCacheSize) {
  // Set up driver with custom max_cache_size = 2
  const std::string yaml_string = R"EOF(
    grpc_service:
      envoy_grpc:
        cluster_name: fake-cluster
      timeout: 0.250s
    max_cache_size: 2
    )EOF";
  envoy::config::trace::v3::OpenTelemetryConfig opentelemetry_config;
  TestUtility::loadFromYaml(yaml_string, opentelemetry_config);
  setup(opentelemetry_config);

  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};

  // set min_flush_spans to 10 avoid automatic flushing.
  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U))
      .Times(2)
      .WillRepeatedly(Return(10));

  // Create first span - should be cached (1/2)
  Tracing::SpanPtr span1 = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                              operation_name_, {Tracing::Reason::Sampling, true});
  EXPECT_NE(span1.get(), nullptr);
  span1->finishSpan();

  // Create second span - should be cached (2/2, at max capacity)
  Tracing::SpanPtr span2 = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                              operation_name_, {Tracing::Reason::Sampling, true});
  EXPECT_NE(span2.get(), nullptr);
  span2->finishSpan();

  // Create another span to trigger flush
  Tracing::SpanPtr trigger_span =
      driver_->startSpan(mock_tracing_config_, request_headers, stream_info_, operation_name_,
                         {Tracing::Reason::Sampling, true});
  EXPECT_NE(trigger_span.get(), nullptr);

  // Should only see 2 spans exported (third span was discarded)
  EXPECT_CALL(*mock_client_, sendRaw(_, _, _, _, _, _));
  trigger_span->finishSpan();

  // Verify only 2 spans were sent (not 3), confirming the third was discarded
  EXPECT_EQ(2U, stats_.counter("tracing.opentelemetry.spans_sent").value());
  // Verify the third span was discarded
  EXPECT_EQ(1U, stats_.counter("tracing.opentelemetry.spans_dropped").value());
}

// Verifies the export happens after a timeout
TEST_F(OpenTelemetryDriverTest, ExportOTLPSpanWithFlushTimeout) {
  timer_ =
      new NiceMock<Event::MockTimer>(&context_.server_factory_context_.thread_local_.dispatcher_);
  ON_CALL(context_.server_factory_context_.thread_local_.dispatcher_, createTimer_(_))
      .WillByDefault(Invoke([this](Event::TimerCb) { return timer_; }));
  EXPECT_CALL(*timer_, enableTimer(std::chrono::milliseconds(5000), _));
  // Set up driver
  setupValidDriver();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};

  Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                             operation_name_, {Tracing::Reason::Sampling, true});
  EXPECT_NE(span.get(), nullptr);

  // Set it to flush after 2 spans so that the span will only be flushed by timeout.
  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U))
      .Times(1)
      .WillRepeatedly(Return(2));
  // We should not yet see a call to sendMessage to export that single span.
  span->finishSpan();
  // Only now should we see the span exported.
  EXPECT_CALL(*mock_client_, sendRaw(_, _, _, _, _, _));
  // Timer should be enabled again.
  EXPECT_CALL(*timer_, enableTimer(std::chrono::milliseconds(5000), _));
  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.flush_interval_ms", 5000U))
      .WillOnce(Return(5000U));
  timer_->invokeCallback();
  EXPECT_EQ(1U, stats_.counter("tracing.opentelemetry.spans_sent").value());
  EXPECT_EQ(1U, stats_.counter("tracing.opentelemetry.timer_flushed").value());
}

// Verifies child span is related to parent span
TEST_F(OpenTelemetryDriverTest, SpawnChildSpan) {
  // Set up driver
  setupValidDriver();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};

  // Mock the random call for generating the parent span's IDs so we can check it later.
  const uint64_t parent_trace_id_high = 0;
  const uint64_t parent_trace_id_low = 2;
  const uint64_t parent_span_id = 3;
  NiceMock<Random::MockRandomGenerator>& mock_random_generator_ =
      context_.server_factory_context_.api_.random_;
  {
    InSequence s;

    EXPECT_CALL(mock_random_generator_, random()).WillOnce(Return(parent_trace_id_high));
    EXPECT_CALL(mock_random_generator_, random()).WillOnce(Return(parent_trace_id_low));
    EXPECT_CALL(mock_random_generator_, random()).WillOnce(Return(parent_span_id));
  }

  Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                             operation_name_, {Tracing::Reason::Sampling, true});
  EXPECT_NE(span.get(), nullptr);

  // The child should only generate a span ID for itself; the trace id should come from the parent..
  const uint64_t child_span_id = 3;
  EXPECT_CALL(mock_random_generator_, random()).WillOnce(Return(child_span_id));
  Tracing::SpanPtr child_span =
      span->spawnChild(mock_tracing_config_, operation_name_, time_system_.systemTime());

  // Flush after a single span.
  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U))
      .Times(1)
      .WillRepeatedly(Return(1));
  // We should see a call to sendMessage to export that single span.
  EXPECT_CALL(*mock_client_, sendRaw(_, _, _, _, _, _));
  child_span->finishSpan();
  EXPECT_EQ(1U, stats_.counter("tracing.opentelemetry.spans_sent").value());
}

// Verifies the OTLP span flags field is populated per the OTLP spec: bits 0-7 carry the W3C
// trace flags (bit 0: sampled), bit 8 records that the parent-remoteness is known, and bit 9
// records whether the parent span context was remote.
TEST_F(OpenTelemetryDriverTest, ExportOTLPSpanFlags) {
  setupValidDriver();

  constexpr uint32_t sampled_flag = 0x1;
  constexpr uint32_t has_is_remote_flag = static_cast<uint32_t>(
      ::opentelemetry::proto::trace::v1::SPAN_FLAGS_CONTEXT_HAS_IS_REMOTE_MASK);
  constexpr uint32_t is_remote_flag =
      static_cast<uint32_t>(::opentelemetry::proto::trace::v1::SPAN_FLAGS_CONTEXT_IS_REMOTE_MASK);

  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};

  {
    // A sampled root span has no remote parent.
    Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                               operation_name_, {Tracing::Reason::Sampling, true});
    span->finishSpan();
    EXPECT_EQ(dynamic_cast<Span*>(span.get())->spanForTest().flags(),
              has_is_remote_flag | sampled_flag);
  }
  {
    // An unsampled span still records that the parent-remoteness is known.
    Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                               operation_name_, {Tracing::Reason::Sampling, true});
    span->setSampled(false);
    span->finishSpan();
    EXPECT_EQ(dynamic_cast<Span*>(span.get())->spanForTest().flags(), has_is_remote_flag);
  }
  {
    // A span spawned from another local span has a local parent.
    Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                               operation_name_, {Tracing::Reason::Sampling, true});
    Tracing::SpanPtr child_span =
        span->spawnChild(mock_tracing_config_, operation_name_, time_system_.systemTime());
    child_span->finishSpan();
    EXPECT_EQ(dynamic_cast<Span*>(child_span.get())->spanForTest().flags(),
              has_is_remote_flag | sampled_flag);
  }
  {
    // A span continuing a trace from an extracted traceparent header has a remote parent.
    Tracing::TestTraceContextImpl request_headers_with_parent{
        {":authority", "test.com"},
        {":path", "/"},
        {":method", "GET"},
        {"traceparent", "00-0000000000000000000000000000000a-"
                        "000000000000000a-01"}};
    Tracing::SpanPtr span =
        driver_->startSpan(mock_tracing_config_, request_headers_with_parent, stream_info_,
                           operation_name_, {Tracing::Reason::Sampling, true});
    span->finishSpan();
    EXPECT_EQ(dynamic_cast<Span*>(span.get())->spanForTest().flags(),
              has_is_remote_flag | is_remote_flag | sampled_flag);
  }
}

// Verifies the span types
TEST_F(OpenTelemetryDriverTest, SpanType) {
  // Set up driver
  setupValidDriver();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};

  // Mock the random call for generating the parent span's IDs so we can check it later.
  const uint64_t parent_trace_id_high = 0;
  const uint64_t parent_trace_id_low = 2;
  const uint64_t parent_span_id = 3;

  {
    mock_tracing_config_.spawn_upstream_span_ = false;
    mock_tracing_config_.operation_name_ = Tracing::OperationName::Ingress;

    NiceMock<Random::MockRandomGenerator>& mock_random_generator_ =
        context_.server_factory_context_.api_.random_;
    {
      InSequence s;

      EXPECT_CALL(mock_random_generator_, random()).WillOnce(Return(parent_trace_id_high));
      EXPECT_CALL(mock_random_generator_, random()).WillOnce(Return(parent_trace_id_low));
      EXPECT_CALL(mock_random_generator_, random()).WillOnce(Return(parent_span_id));
    }

    Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                               operation_name_, {Tracing::Reason::Sampling, true});
    EXPECT_NE(span.get(), nullptr);

    // The span type should be SERVER because spawn_upstream_span_ is false and operation_name_ is
    // Ingress.
    EXPECT_EQ(dynamic_cast<Span*>(span.get())->spanForTest().kind(),
              ::opentelemetry::proto::trace::v1::Span::SPAN_KIND_SERVER);

    // The child should only generate a span ID for itself; the trace id should come from the
    // parent.
    const uint64_t child_span_id = 3;
    EXPECT_CALL(mock_random_generator_, random()).WillOnce(Return(child_span_id));
    Tracing::SpanPtr child_span =
        span->spawnChild(mock_tracing_config_, operation_name_, time_system_.systemTime());

    // The child span should also be a CLIENT span.
    EXPECT_EQ(dynamic_cast<Span*>(child_span.get())->spanForTest().kind(),
              ::opentelemetry::proto::trace::v1::Span::SPAN_KIND_CLIENT);
  }

  {
    mock_tracing_config_.spawn_upstream_span_ = false;
    mock_tracing_config_.operation_name_ = Tracing::OperationName::Egress;

    NiceMock<Random::MockRandomGenerator>& mock_random_generator_ =
        context_.server_factory_context_.api_.random_;
    {
      InSequence s;

      EXPECT_CALL(mock_random_generator_, random()).WillOnce(Return(parent_trace_id_high));
      EXPECT_CALL(mock_random_generator_, random()).WillOnce(Return(parent_trace_id_low));
      EXPECT_CALL(mock_random_generator_, random()).WillOnce(Return(parent_span_id));
    }

    Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                               operation_name_, {Tracing::Reason::Sampling, true});
    EXPECT_NE(span.get(), nullptr);

    // The span type should be CLIENT because spawn_upstream_span_ is false and operation_name_ is
    // Egress.
    EXPECT_EQ(dynamic_cast<Span*>(span.get())->spanForTest().kind(),
              ::opentelemetry::proto::trace::v1::Span::SPAN_KIND_CLIENT);

    // The child should only generate a span ID for itself; the trace id should come from the
    // parent.
    const uint64_t child_span_id = 3;
    EXPECT_CALL(mock_random_generator_, random()).WillOnce(Return(child_span_id));
    Tracing::SpanPtr child_span =
        span->spawnChild(mock_tracing_config_, operation_name_, time_system_.systemTime());

    // The child span should also be a CLIENT span.
    EXPECT_EQ(dynamic_cast<Span*>(child_span.get())->spanForTest().kind(),
              ::opentelemetry::proto::trace::v1::Span::SPAN_KIND_CLIENT);
  }

  {
    mock_tracing_config_.spawn_upstream_span_ = true;
    mock_tracing_config_.operation_name_ = Tracing::OperationName::Egress;

    NiceMock<Random::MockRandomGenerator>& mock_random_generator_ =
        context_.server_factory_context_.api_.random_;
    {
      InSequence s;

      EXPECT_CALL(mock_random_generator_, random()).WillOnce(Return(parent_trace_id_high));
      EXPECT_CALL(mock_random_generator_, random()).WillOnce(Return(parent_trace_id_low));
      EXPECT_CALL(mock_random_generator_, random()).WillOnce(Return(parent_span_id));
    }

    Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                               operation_name_, {Tracing::Reason::Sampling, true});
    EXPECT_NE(span.get(), nullptr);

    // The span type should be SERVER because spawn_upstream_span_ is true and this is
    // downstream span.
    EXPECT_EQ(dynamic_cast<Span*>(span.get())->spanForTest().kind(),
              ::opentelemetry::proto::trace::v1::Span::SPAN_KIND_SERVER);

    // The child should only generate a span ID for itself; the trace id should come from the
    // parent.
    const uint64_t child_span_id = 3;
    EXPECT_CALL(mock_random_generator_, random()).WillOnce(Return(child_span_id));
    Tracing::SpanPtr child_span =
        span->spawnChild(mock_tracing_config_, operation_name_, time_system_.systemTime());

    child_span->setTag("http.status_code", "400");

    // The child span should also be a CLIENT span.
    EXPECT_EQ(dynamic_cast<Span*>(child_span.get())->spanForTest().kind(),
              ::opentelemetry::proto::trace::v1::Span::SPAN_KIND_CLIENT);
    // The child span should have an error status.
    EXPECT_EQ(dynamic_cast<Span*>(child_span.get())->spanForTest().status().code(),
              ::opentelemetry::proto::trace::v1::Status::STATUS_CODE_ERROR);
  }
}

// Verifies spans are exported with their attributes
TEST_F(OpenTelemetryDriverTest, ExportOTLPSpanWithAttributes) {
  setupValidDriver();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};
  NiceMock<Random::MockRandomGenerator>& mock_random_generator_ =
      context_.server_factory_context_.api_.random_;
  int64_t generated_int = 1;
  EXPECT_CALL(mock_random_generator_, random()).Times(3).WillRepeatedly(Return(generated_int));
  SystemTime timestamp = time_system_.systemTime();
  ON_CALL(stream_info_, startTime()).WillByDefault(Return(timestamp));

  Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                             operation_name_, {Tracing::Reason::Sampling, true});
  EXPECT_NE(span.get(), nullptr);

  span->setTag("first_tag_name", "first_tag_value");
  span->setTag("second_tag_name", "second_tag_value");
  // Try an empty tag.
  span->setTag("", "empty_tag_value");
  // Overwrite a tag.
  span->setTag("first_tag_name", "first_tag_new_value");

  // Note the placeholders for the bytes - cleaner to manually set after.
  constexpr absl::string_view request_yaml = R"(
resource_spans:
  resource:
    attributes:
      key: "service.name"
      value:
        string_value: "unknown_service:envoy"
      key: "key1"
      value:
        string_value: "val1"
  scope_spans:
    scope:
      name: "envoy"
      version: {}
    spans:
      trace_id: "AAA"
      span_id: "AAA"
      name: "test"
      kind: SPAN_KIND_SERVER
      start_time_unix_nano: {}
      end_time_unix_nano: {}
      flags: 257
      attributes:
        - key: "first_tag_name"
          value:
            string_value: "first_tag_new_value"
        - key: "second_tag_name"
          value:
            string_value: "second_tag_value"
  )";
  opentelemetry::proto::collector::trace::v1::ExportTraceServiceRequest request_proto;
  int64_t timestamp_ns = std::chrono::nanoseconds(timestamp.time_since_epoch()).count();
  absl::string_view envoy_version = Envoy::VersionInfo::version();

  TestUtility::loadFromYaml(fmt::format(request_yaml, envoy_version, timestamp_ns, timestamp_ns),
                            request_proto);
  std::string generated_int_hex = Hex::uint64ToHex(generated_int);
  auto* expected_span =
      request_proto.mutable_resource_spans(0)->mutable_scope_spans(0)->mutable_spans(0);
  expected_span->set_trace_id(
      absl::HexStringToBytes(absl::StrCat(generated_int_hex, generated_int_hex)));
  expected_span->set_span_id(absl::HexStringToBytes(absl::StrCat(generated_int_hex)));

  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U))
      .Times(1)
      .WillRepeatedly(Return(1));
  EXPECT_CALL(
      *mock_client_,
      sendRaw(_, _, Grpc::ProtoBufferEqIgnoreRepeatedFieldOrdering(request_proto), _, _, _));
  span->finishSpan();
  EXPECT_EQ(1U, stats_.counter("tracing.opentelemetry.spans_sent").value());
}

// Verifies typed span attributes are exported as native OTLP values
TEST_F(OpenTelemetryDriverTest, ExportOTLPSpanWithTypedAttributes) {
  setupValidDriver();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};
  NiceMock<Random::MockRandomGenerator>& mock_random_generator_ =
      context_.server_factory_context_.api_.random_;
  int64_t generated_int = 1;
  EXPECT_CALL(mock_random_generator_, random()).Times(3).WillRepeatedly(Return(generated_int));
  SystemTime timestamp = time_system_.systemTime();
  ON_CALL(stream_info_, startTime()).WillByDefault(Return(timestamp));

  Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                             operation_name_, {Tracing::Reason::Sampling, true});
  EXPECT_NE(span.get(), nullptr);

  span->setTypedTag("int_tag", "42", Tracing::TagValueType::Int);
  span->setTypedTag("double_tag", "3.5", Tracing::TagValueType::Double);
  span->setTypedTag("bool_tag", "true", Tracing::TagValueType::Bool);
  span->setTypedTag("string_tag", "hello", Tracing::TagValueType::String);
  // A value that does not parse to the requested type falls back to a string attribute.
  span->setTypedTag("fallback_tag", "not-a-number", Tracing::TagValueType::Int);

  constexpr absl::string_view request_yaml = R"(
resource_spans:
  resource:
    attributes:
      key: "service.name"
      value:
        string_value: "unknown_service:envoy"
      key: "key1"
      value:
        string_value: "val1"
  scope_spans:
    scope:
      name: "envoy"
      version: {}
    spans:
      trace_id: "AAA"
      span_id: "AAA"
      name: "test"
      kind: SPAN_KIND_SERVER
      start_time_unix_nano: {}
      end_time_unix_nano: {}
      flags: 257
      attributes:
        - key: "int_tag"
          value:
            int_value: 42
        - key: "double_tag"
          value:
            double_value: 3.5
        - key: "bool_tag"
          value:
            bool_value: true
        - key: "string_tag"
          value:
            string_value: "hello"
        - key: "fallback_tag"
          value:
            string_value: "not-a-number"
  )";
  opentelemetry::proto::collector::trace::v1::ExportTraceServiceRequest request_proto;
  int64_t timestamp_ns = std::chrono::nanoseconds(timestamp.time_since_epoch()).count();
  absl::string_view envoy_version = Envoy::VersionInfo::version();

  TestUtility::loadFromYaml(fmt::format(request_yaml, envoy_version, timestamp_ns, timestamp_ns),
                            request_proto);
  std::string generated_int_hex = Hex::uint64ToHex(generated_int);
  auto* expected_span =
      request_proto.mutable_resource_spans(0)->mutable_scope_spans(0)->mutable_spans(0);
  expected_span->set_trace_id(
      absl::HexStringToBytes(absl::StrCat(generated_int_hex, generated_int_hex)));
  expected_span->set_span_id(absl::HexStringToBytes(absl::StrCat(generated_int_hex)));

  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U))
      .Times(1)
      .WillRepeatedly(Return(1));
  EXPECT_CALL(
      *mock_client_,
      sendRaw(_, _, Grpc::ProtoBufferEqIgnoreRepeatedFieldOrdering(request_proto), _, _, _));
  span->finishSpan();
  EXPECT_EQ(1U, stats_.counter("tracing.opentelemetry.spans_sent").value());
}

TEST_F(OpenTelemetryDriverTest, ExportOTLPSpanWithBatchAttributes) {
  setupValidDriver();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};
  NiceMock<Random::MockRandomGenerator>& mock_random_generator_ =
      context_.server_factory_context_.api_.random_;
  int64_t generated_int = 1;
  EXPECT_CALL(mock_random_generator_, random()).Times(3).WillRepeatedly(Return(generated_int));
  SystemTime timestamp = time_system_.systemTime();
  ON_CALL(stream_info_, startTime()).WillByDefault(Return(timestamp));

  Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                             operation_name_, {Tracing::Reason::Sampling, true});
  EXPECT_NE(span.get(), nullptr);

  span->reserveTags(2);
  span->setTag("first_tag_name", "first_tag_value");
  span->setTag("second_tag_name", "second_tag_value");

  constexpr absl::string_view request_yaml = R"(
resource_spans:
  resource:
    attributes:
      key: "service.name"
      value:
        string_value: "unknown_service:envoy"
      key: "key1"
      value:
        string_value: "val1"
  scope_spans:
    scope:
      name: "envoy"
      version: {}
    spans:
      trace_id: "AAA"
      span_id: "AAA"
      name: "test"
      kind: SPAN_KIND_SERVER
      start_time_unix_nano: {}
      end_time_unix_nano: {}
      flags: 257
      attributes:
        - key: "first_tag_name"
          value:
            string_value: "first_tag_value"
        - key: "second_tag_name"
          value:
            string_value: "second_tag_value"
  )";
  opentelemetry::proto::collector::trace::v1::ExportTraceServiceRequest request_proto;
  int64_t timestamp_ns = std::chrono::nanoseconds(timestamp.time_since_epoch()).count();
  absl::string_view envoy_version = Envoy::VersionInfo::version();

  TestUtility::loadFromYaml(fmt::format(request_yaml, envoy_version, timestamp_ns, timestamp_ns),
                            request_proto);
  std::string generated_int_hex = Hex::uint64ToHex(generated_int);
  auto* expected_span =
      request_proto.mutable_resource_spans(0)->mutable_scope_spans(0)->mutable_spans(0);
  expected_span->set_trace_id(
      absl::HexStringToBytes(absl::StrCat(generated_int_hex, generated_int_hex)));
  expected_span->set_span_id(absl::HexStringToBytes(absl::StrCat(generated_int_hex)));

  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U))
      .Times(1)
      .WillRepeatedly(Return(1));
  EXPECT_CALL(
      *mock_client_,
      sendRaw(_, _, Grpc::ProtoBufferEqIgnoreRepeatedFieldOrdering(request_proto), _, _, _));
  span->finishSpan();
  EXPECT_EQ(1U, stats_.counter("tracing.opentelemetry.spans_sent").value());
}

// Verifies spans are exported with known HTTP method
TEST_F(OpenTelemetryDriverTest, ExportOTLPSpanWithKnownMethod) {
  setupValidDriver();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};
  NiceMock<Random::MockRandomGenerator>& mock_random_generator_ =
      context_.server_factory_context_.api_.random_;
  int64_t generated_int = 1;
  EXPECT_CALL(mock_random_generator_, random()).Times(3).WillRepeatedly(Return(generated_int));
  SystemTime timestamp = time_system_.systemTime();
  ON_CALL(stream_info_, startTime()).WillByDefault(Return(timestamp));

  Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                             operation_name_, {Tracing::Reason::Sampling, true});
  EXPECT_NE(span.get(), nullptr);

  span->setTag("http.method", "GET");

  constexpr absl::string_view request_yaml = R"(
resource_spans:
  resource:
    attributes:
      key: "service.name"
      value:
        string_value: "unknown_service:envoy"
      key: "key1"
      value:
        string_value: "val1"
  scope_spans:
    scope:
      name: "envoy"
      version: {}
    spans:
      trace_id: "AAA"
      span_id: "AAA"
      name: "test"
      kind: SPAN_KIND_SERVER
      start_time_unix_nano: {}
      end_time_unix_nano: {}
      flags: 257
      attributes:
        - key: "http.method"
          value:
            string_value: "GET"
  )";
  opentelemetry::proto::collector::trace::v1::ExportTraceServiceRequest request_proto;
  int64_t timestamp_ns = std::chrono::nanoseconds(timestamp.time_since_epoch()).count();
  absl::string_view envoy_version = Envoy::VersionInfo::version();

  TestUtility::loadFromYaml(fmt::format(request_yaml, envoy_version, timestamp_ns, timestamp_ns),
                            request_proto);
  std::string generated_int_hex = Hex::uint64ToHex(generated_int);
  auto* expected_span =
      request_proto.mutable_resource_spans(0)->mutable_scope_spans(0)->mutable_spans(0);
  expected_span->set_trace_id(
      absl::HexStringToBytes(absl::StrCat(generated_int_hex, generated_int_hex)));
  expected_span->set_span_id(absl::HexStringToBytes(absl::StrCat(generated_int_hex)));

  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U))
      .Times(1)
      .WillRepeatedly(Return(1));
  EXPECT_CALL(
      *mock_client_,
      sendRaw(_, _, Grpc::ProtoBufferEqIgnoreRepeatedFieldOrdering(request_proto), _, _, _));
  span->finishSpan();
  EXPECT_EQ(1U, stats_.counter("tracing.opentelemetry.spans_sent").value());
}

// Verifies spans are exported with unknown HTTP method mapped to _OTHER
TEST_F(OpenTelemetryDriverTest, ExportOTLPSpanWithUnknownMethod) {
  setupValidDriver();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};
  NiceMock<Random::MockRandomGenerator>& mock_random_generator_ =
      context_.server_factory_context_.api_.random_;
  int64_t generated_int = 1;
  EXPECT_CALL(mock_random_generator_, random()).Times(3).WillRepeatedly(Return(generated_int));
  SystemTime timestamp = time_system_.systemTime();
  ON_CALL(stream_info_, startTime()).WillByDefault(Return(timestamp));

  Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                             operation_name_, {Tracing::Reason::Sampling, true});
  EXPECT_NE(span.get(), nullptr);

  span->setTag("http.method", "PURGE");

  constexpr absl::string_view request_yaml = R"(
resource_spans:
  resource:
    attributes:
      key: "service.name"
      value:
        string_value: "unknown_service:envoy"
      key: "key1"
      value:
        string_value: "val1"
  scope_spans:
    scope:
      name: "envoy"
      version: {}
    spans:
      trace_id: "AAA"
      span_id: "AAA"
      name: "test"
      kind: SPAN_KIND_SERVER
      start_time_unix_nano: {}
      end_time_unix_nano: {}
      flags: 257
      attributes:
        - key: "http.method"
          value:
            string_value: "_OTHER"
  )";
  opentelemetry::proto::collector::trace::v1::ExportTraceServiceRequest request_proto;
  int64_t timestamp_ns = std::chrono::nanoseconds(timestamp.time_since_epoch()).count();
  absl::string_view envoy_version = Envoy::VersionInfo::version();

  TestUtility::loadFromYaml(fmt::format(request_yaml, envoy_version, timestamp_ns, timestamp_ns),
                            request_proto);
  std::string generated_int_hex = Hex::uint64ToHex(generated_int);
  auto* expected_span =
      request_proto.mutable_resource_spans(0)->mutable_scope_spans(0)->mutable_spans(0);
  expected_span->set_trace_id(
      absl::HexStringToBytes(absl::StrCat(generated_int_hex, generated_int_hex)));
  expected_span->set_span_id(absl::HexStringToBytes(absl::StrCat(generated_int_hex)));

  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U))
      .Times(1)
      .WillRepeatedly(Return(1));
  EXPECT_CALL(
      *mock_client_,
      sendRaw(_, _, Grpc::ProtoBufferEqIgnoreRepeatedFieldOrdering(request_proto), _, _, _));
  span->finishSpan();
  EXPECT_EQ(1U, stats_.counter("tracing.opentelemetry.spans_sent").value());
}

// Verifies spans are exported with their events
TEST_F(OpenTelemetryDriverTest, ExportOTLPSpanWithEvents) {
  setupValidDriver();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};
  NiceMock<Random::MockRandomGenerator>& mock_random_generator_ =
      context_.server_factory_context_.api_.random_;
  int64_t generated_int = 1;
  EXPECT_CALL(mock_random_generator_, random()).Times(3).WillRepeatedly(Return(generated_int));
  SystemTime timestamp = time_system_.systemTime();
  ON_CALL(stream_info_, startTime()).WillByDefault(Return(timestamp));

  Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                             operation_name_, {Tracing::Reason::Sampling, true});
  EXPECT_NE(span.get(), nullptr);

  span->log(SystemTime(), "");
  span->log(SystemTime(SystemTime::duration(Event::TimeSystem::Milliseconds(1))), "event1");
  span->log(SystemTime(SystemTime::duration(Event::TimeSystem::Milliseconds(2))), "event2");

  // Note the placeholders for the bytes - cleaner to manually set after.
  constexpr absl::string_view request_yaml = R"(
resource_spans:
  resource:
    attributes:
      key: "service.name"
      value:
        string_value: "unknown_service:envoy"
      key: "key1"
      value:
        string_value: "val1"
  scope_spans:
    scope:
      name: "envoy"
      version: {}
    spans:
      trace_id: "AAA"
      span_id: "AAA"
      name: "test"
      kind: SPAN_KIND_SERVER
      start_time_unix_nano: {}
      end_time_unix_nano: {}
      flags: 257
      events:
        - name: event1
          time_unix_nano: 1000000
        - name: event2
          time_unix_nano: 2000000
  )";
  opentelemetry::proto::collector::trace::v1::ExportTraceServiceRequest request_proto;
  int64_t timestamp_ns = std::chrono::nanoseconds(timestamp.time_since_epoch()).count();
  absl::string_view envoy_version = Envoy::VersionInfo::version();

  TestUtility::loadFromYaml(fmt::format(request_yaml, envoy_version, timestamp_ns, timestamp_ns),
                            request_proto);
  std::string generated_int_hex = Hex::uint64ToHex(generated_int);
  auto* expected_span =
      request_proto.mutable_resource_spans(0)->mutable_scope_spans(0)->mutable_spans(0);
  expected_span->set_trace_id(
      absl::HexStringToBytes(absl::StrCat(generated_int_hex, generated_int_hex)));
  expected_span->set_span_id(absl::HexStringToBytes(absl::StrCat(generated_int_hex)));

  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U))
      .Times(1)
      .WillRepeatedly(Return(1));
  EXPECT_CALL(
      *mock_client_,
      sendRaw(_, _, Grpc::ProtoBufferEqIgnoreRepeatedFieldOrdering(request_proto), _, _, _));
  span->finishSpan();
  EXPECT_EQ(1U, stats_.counter("tracing.opentelemetry.spans_sent").value());
}

// Verifies spans are exported with their attributes and status
TEST_F(OpenTelemetryDriverTest, ExportOTLPSpanWithAttributesAndStatus) {
  setupValidDriver();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};
  NiceMock<Random::MockRandomGenerator>& mock_random_generator_ =
      context_.server_factory_context_.api_.random_;
  int64_t generated_int = 1;
  EXPECT_CALL(mock_random_generator_, random()).Times(3).WillRepeatedly(Return(generated_int));
  SystemTime timestamp = time_system_.systemTime();
  ON_CALL(stream_info_, startTime()).WillByDefault(Return(timestamp));

  Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                             operation_name_, {Tracing::Reason::Sampling, true});
  EXPECT_NE(span.get(), nullptr);

  span->setTag("first_tag_name", "first_tag_value");
  span->setTag("second_tag_name", "second_tag_value");
  // Try an empty tag.
  span->setTag("", "empty_tag_value");
  // Overwrite a tag.
  span->setTag("first_tag_name", "first_tag_new_value");
  span->setTag("http.status_code", "500");

  // Note the placeholders for the bytes - cleaner to manually set after.
  constexpr absl::string_view request_yaml = R"(
resource_spans:
  resource:
    attributes:
      key: "service.name"
      value:
        string_value: "unknown_service:envoy"
      key: "key1"
      value:
        string_value: "val1"
  scope_spans:
    scope:
      name: "envoy"
      version: {}
    spans:
      trace_id: "AAA"
      span_id: "AAA"
      name: "test"
      kind: SPAN_KIND_SERVER
      start_time_unix_nano: {}
      end_time_unix_nano: {}
      flags: 257
      status:
        code: STATUS_CODE_ERROR
      attributes:
        - key: "first_tag_name"
          value:
            string_value: "first_tag_new_value"
        - key: "second_tag_name"
          value:
            string_value: "second_tag_value"
        - key: "http.status_code"
          value:
            string_value: "500"
  )";
  opentelemetry::proto::collector::trace::v1::ExportTraceServiceRequest request_proto;
  int64_t timestamp_ns = std::chrono::nanoseconds(timestamp.time_since_epoch()).count();
  absl::string_view envoy_version = Envoy::VersionInfo::version();

  TestUtility::loadFromYaml(fmt::format(request_yaml, envoy_version, timestamp_ns, timestamp_ns),
                            request_proto);
  std::string generated_int_hex = Hex::uint64ToHex(generated_int);
  auto* expected_span =
      request_proto.mutable_resource_spans(0)->mutable_scope_spans(0)->mutable_spans(0);
  expected_span->set_trace_id(
      absl::HexStringToBytes(absl::StrCat(generated_int_hex, generated_int_hex)));
  expected_span->set_span_id(absl::HexStringToBytes(absl::StrCat(generated_int_hex)));

  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U))
      .Times(1)
      .WillRepeatedly(Return(1));
  EXPECT_CALL(
      *mock_client_,
      sendRaw(_, _, Grpc::ProtoBufferEqIgnoreRepeatedFieldOrdering(request_proto), _, _, _));
  span->finishSpan();
  EXPECT_EQ(1U, stats_.counter("tracing.opentelemetry.spans_sent").value());
}

// Verifies Grpc spans are exported with their attributes and status
TEST_F(OpenTelemetryDriverTest, ExportOTLPGRPCSpanWithAttributesAndStatus) {
  setupValidDriver();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};
  NiceMock<Random::MockRandomGenerator>& mock_random_generator_ =
      context_.server_factory_context_.api_.random_;
  int64_t generated_int = 1;
  EXPECT_CALL(mock_random_generator_, random()).Times(3).WillRepeatedly(Return(generated_int));
  SystemTime timestamp = time_system_.systemTime();
  ON_CALL(stream_info_, startTime()).WillByDefault(Return(timestamp));

  Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                             operation_name_, {Tracing::Reason::Sampling, true});
  EXPECT_NE(span.get(), nullptr);

  span->setTag("first_tag_name", "first_tag_value");
  span->setTag("second_tag_name", "second_tag_value");
  // Try an empty tag.
  span->setTag("", "empty_tag_value");
  // Overwrite a tag.
  span->setTag("first_tag_name", "first_tag_new_value");
  span->setTag("http.status_code", "200");
  span->setTag("grpc.status_code", "13");
  span->setTag("grpc.message", "connect Canceled randomly");

  // Note the placeholders for the bytes - cleaner to manually set after.
  constexpr absl::string_view request_yaml = R"(
resource_spans:
  resource:
    attributes:
      key: "service.name"
      value:
        string_value: "unknown_service:envoy"
      key: "key1"
      value:
        string_value: "val1"
  scope_spans:
    scope:
      name: "envoy"
      version: {}
    spans:
      trace_id: "AAA"
      span_id: "AAA"
      name: "test"
      kind: SPAN_KIND_SERVER
      start_time_unix_nano: {}
      end_time_unix_nano: {}
      flags: 257
      status:
        code: STATUS_CODE_ERROR
      attributes:
        - key: "first_tag_name"
          value:
            string_value: "first_tag_new_value"
        - key: "second_tag_name"
          value:
            string_value: "second_tag_value"
        - key: "http.status_code"
          value:
            string_value: "200"
        - key: "grpc.status_code"
          value:
            string_value: "13"
        - key: "grpc.message"
          value:
            string_value: "connect Canceled randomly"
  )";
  opentelemetry::proto::collector::trace::v1::ExportTraceServiceRequest request_proto;
  int64_t timestamp_ns = std::chrono::nanoseconds(timestamp.time_since_epoch()).count();
  absl::string_view envoy_version = Envoy::VersionInfo::version();

  TestUtility::loadFromYaml(fmt::format(request_yaml, envoy_version, timestamp_ns, timestamp_ns),
                            request_proto);
  std::string generated_int_hex = Hex::uint64ToHex(generated_int);
  auto* expected_span =
      request_proto.mutable_resource_spans(0)->mutable_scope_spans(0)->mutable_spans(0);
  expected_span->set_trace_id(
      absl::HexStringToBytes(absl::StrCat(generated_int_hex, generated_int_hex)));
  expected_span->set_span_id(absl::HexStringToBytes(absl::StrCat(generated_int_hex)));

  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U))
      .Times(1)
      .WillRepeatedly(Return(1));
  EXPECT_CALL(
      *mock_client_,
      sendRaw(_, _, Grpc::ProtoBufferEqIgnoreRepeatedFieldOrdering(request_proto), _, _, _));
  span->finishSpan();
  EXPECT_EQ(1U, stats_.counter("tracing.opentelemetry.spans_sent").value());
}

// Not sampled spans are ignored
TEST_F(OpenTelemetryDriverTest, IgnoreNotSampledSpan) {
  setupValidDriver();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};
  Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                             operation_name_, {Tracing::Reason::Sampling, true});
  EXPECT_NE(span.get(), nullptr);

  span->setSampled(false);

  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U)).Times(0);
  EXPECT_CALL(*mock_client_, sendRaw(_, _, _, _, _, _)).Times(0);
  span->finishSpan();
  EXPECT_EQ(0U, stats_.counter("tracing.opentelemetry.spans_sent").value());
}

TEST_F(OpenTelemetryDriverTest, UseLocalDecisionTrue) {
  setupValidDriver();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};

  Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                             operation_name_, {Tracing::Reason::Sampling, true});
  // The `useLocalDecision` should be true because there is no traceparent header in the request.
  EXPECT_TRUE(span->useLocalDecision());

  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U))
      .Times(1)
      .WillRepeatedly(Return(1));
  EXPECT_CALL(*mock_client_, sendRaw(_, _, _, _, _, _));
  span->finishSpan();
  EXPECT_EQ(1U, stats_.counter("tracing.opentelemetry.spans_sent").value());
}

TEST_F(OpenTelemetryDriverTest, UseLocalDecisionFalse) {
  setupValidDriver();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"},
      {":path", "/"},
      {":method", "GET"},
      {"traceparent", "00-00000000000000010000000000000002-0000000000000003-01"}};

  // A propagated parent disables local-decision mode, but the request-entry tracing decision
  // still acts as a final veto.
  Tracing::SpanPtr span =
      driver_->startSpan(mock_tracing_config_, request_headers, stream_info_, operation_name_,
                         {Tracing::Reason::NotTraceable, false});
  // The `useLocalDecision` should be false because there is a traceparent header in the request.
  EXPECT_FALSE(span->useLocalDecision());
  EXPECT_FALSE(span->exportedSpan());

  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U)).Times(0);
  EXPECT_CALL(*mock_client_, sendRaw(_, _, _, _, _, _)).Times(0);
  span->finishSpan();
  EXPECT_EQ(0U, stats_.counter("tracing.opentelemetry.spans_sent").value());
}

TEST_F(OpenTelemetryDriverTest, RootSpanWithAlwaysOnSamplerStillHonorsEnvoyDecision) {
  setupValidDriverWithAlwaysOnSampler();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};

  Tracing::SpanPtr span =
      driver_->startSpan(mock_tracing_config_, request_headers, stream_info_, operation_name_,
                         {Tracing::Reason::NotTraceable, false});

  EXPECT_FALSE(span->useLocalDecision());
  EXPECT_FALSE(dynamic_cast<Span*>(span.get())->sampled());

  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U)).Times(0);
  EXPECT_CALL(*mock_client_, sendRaw(_, _, _, _, _, _)).Times(0);
  span->finishSpan();
  EXPECT_EQ(0U, stats_.counter("tracing.opentelemetry.spans_sent").value());
}

TEST_F(OpenTelemetryDriverTest, PropagatedSpanWithAlwaysOnSamplerStillHonorsEnvoyDecision) {
  setupValidDriverWithAlwaysOnSampler();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"},
      {":path", "/"},
      {":method", "GET"},
      {"traceparent", "00-00000000000000010000000000000002-0000000000000003-01"}};

  Tracing::SpanPtr span =
      driver_->startSpan(mock_tracing_config_, request_headers, stream_info_, operation_name_,
                         {Tracing::Reason::NotTraceable, false});

  EXPECT_FALSE(span->useLocalDecision());
  EXPECT_FALSE(dynamic_cast<Span*>(span.get())->sampled());

  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U)).Times(0);
  EXPECT_CALL(*mock_client_, sendRaw(_, _, _, _, _, _)).Times(0);
  span->finishSpan();
  EXPECT_EQ(0U, stats_.counter("tracing.opentelemetry.spans_sent").value());
}

TEST_F(OpenTelemetryDriverTest, AlwaysOnSamplerExportsWhenEnvoyDecisionAllowsTracing) {
  setupValidDriverWithAlwaysOnSampler();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"},
      {":path", "/"},
      {":method", "GET"},
      {"traceparent", "00-00000000000000010000000000000002-0000000000000003-01"}};

  Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                             operation_name_, {Tracing::Reason::Sampling, true});

  EXPECT_FALSE(span->useLocalDecision());
  EXPECT_TRUE(dynamic_cast<Span*>(span.get())->sampled());

  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U))
      .Times(1)
      .WillRepeatedly(Return(1));
  EXPECT_CALL(*mock_client_, sendRaw(_, _, _, _, _, _));
  span->finishSpan();
  EXPECT_EQ(1U, stats_.counter("tracing.opentelemetry.spans_sent").value());
}

// Disabling the local decision keeps a setSampled(false) decision and drops the span.
TEST_F(OpenTelemetryDriverTest, DisableLocalDecisionWithSampledFalse) {
  setupValidDriver();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};

  Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                             operation_name_, {Tracing::Reason::Sampling, true});
  // The span starts on the local decision and is sampled because there is no traceparent header.
  EXPECT_TRUE(span->useLocalDecision());
  EXPECT_TRUE(dynamic_cast<Span*>(span.get())->sampled());

  span->setSampled(false);
  span->disableLocalDecision();
  // Disabling the local decision stops the connection manager from re-deriving the decision.
  EXPECT_FALSE(span->useLocalDecision());
  EXPECT_FALSE(dynamic_cast<Span*>(span.get())->sampled());

  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U)).Times(0);
  EXPECT_CALL(*mock_client_, sendRaw(_, _, _, _, _, _)).Times(0);
  span->finishSpan();
  EXPECT_EQ(0U, stats_.counter("tracing.opentelemetry.spans_sent").value());
}

// Disabling the local decision keeps a setSampled(true) decision and keeps the span.
TEST_F(OpenTelemetryDriverTest, DisableLocalDecisionWithSampledTrue) {
  setupValidDriver();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};

  Tracing::SpanPtr span =
      driver_->startSpan(mock_tracing_config_, request_headers, stream_info_, operation_name_,
                         {Tracing::Reason::NotTraceable, false});
  // The span starts on the local decision and is unsampled because there is no traceparent header.
  EXPECT_TRUE(span->useLocalDecision());
  EXPECT_FALSE(span->exportedSpan());

  span->setSampled(true);
  span->disableLocalDecision();
  EXPECT_FALSE(span->useLocalDecision());
  EXPECT_TRUE(span->exportedSpan());

  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U))
      .Times(1)
      .WillRepeatedly(Return(1));
  EXPECT_CALL(*mock_client_, sendRaw(_, _, _, _, _, _));
  span->finishSpan();
  EXPECT_EQ(1U, stats_.counter("tracing.opentelemetry.spans_sent").value());
}

// Spans that are not sampled, with the runtime guard for deferred spans on and off. Every
// expectation on ids, propagated headers and exported spans is the same for both values: only the
// class of the span differs.
class OpenTelemetryDeferredSpanTest : public OpenTelemetryDriverTest,
                                      public testing::WithParamInterface<bool> {
public:
  OpenTelemetryDeferredSpanTest() {
    setGuard(GetParam());
    ON_CALL(stream_info_, startTime()).WillByDefault(Return(timestamp_));
  }

  void setGuard(bool enabled) {
    scoped_runtime_.mergeValues(
        {{"envoy.reloadable_features.otel_tracer_deferred_span", enabled ? "true" : "false"}});
  }

  // Whether the span is a DeferredSpan.
  static bool deferredSpan(const Tracing::Span& span) {
    return dynamic_cast<const DeferredSpan*>(&span) != nullptr;
  }

  // Whether the span is a DeferredSpan that has not built its OTLP span.
  static bool notBuilt(const Tracing::Span& span) {
    const auto* deferred_span = dynamic_cast<const DeferredSpan*>(&span);
    return deferred_span != nullptr && deferred_span->recordingSpanForTest() == nullptr;
  }

  // The OTLP span behind the span, with either class. Null while a DeferredSpan has not built it.
  static const ::opentelemetry::proto::trace::v1::Span* otlpSpan(const Tracing::Span& span) {
    if (const auto* deferred_span = dynamic_cast<const DeferredSpan*>(&span)) {
      return deferred_span->recordingSpanForTest() != nullptr
                 ? &deferred_span->recordingSpanForTest()->spanForTest()
                 : nullptr;
    }
    return &dynamic_cast<const Span&>(span).spanForTest();
  }

  void expectRandom(std::vector<uint64_t> values) {
    NiceMock<Random::MockRandomGenerator>& random = context_.server_factory_context_.api_.random_;
    InSequence s;
    for (const uint64_t value : values) {
      EXPECT_CALL(random, random()).WillOnce(Return(value));
    }
  }

  void expectNothingExportedOnFinish(Tracing::Span& span) {
    EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U))
        .Times(0);
    EXPECT_CALL(*mock_client_, sendRaw(_, _, _, _, _, _)).Times(0);
    span.finishSpan();
    EXPECT_EQ(0U, stats_.counter("tracing.opentelemetry.spans_sent").value());
  }

  // Expects one export request with one span that has the given fields.
  void expectExport(const std::string& span_fields_yaml, absl::string_view trace_id_hex,
                    absl::string_view span_id_hex, absl::string_view parent_span_id_hex,
                    SystemTime start_time) {
    constexpr absl::string_view request_yaml = R"(
resource_spans:
  resource:
    attributes:
      key: "service.name"
      value:
        string_value: "unknown_service:envoy"
      key: "key1"
      value:
        string_value: "val1"
  scope_spans:
    scope:
      name: "envoy"
      version: {}
    spans:
      trace_id: "AAA"
      span_id: "AAA"
      start_time_unix_nano: {}
      end_time_unix_nano: {}
{}
  )";
    opentelemetry::proto::collector::trace::v1::ExportTraceServiceRequest request_proto;
    const int64_t start_ns = std::chrono::nanoseconds(start_time.time_since_epoch()).count();
    const int64_t end_ns =
        std::chrono::nanoseconds(time_system_.systemTime().time_since_epoch()).count();
    TestUtility::loadFromYaml(fmt::format(request_yaml, Envoy::VersionInfo::version(), start_ns,
                                          end_ns, span_fields_yaml),
                              request_proto);
    auto* expected_span =
        request_proto.mutable_resource_spans(0)->mutable_scope_spans(0)->mutable_spans(0);
    expected_span->set_trace_id(absl::HexStringToBytes(trace_id_hex));
    expected_span->set_span_id(absl::HexStringToBytes(span_id_hex));
    if (!parent_span_id_hex.empty()) {
      expected_span->set_parent_span_id(absl::HexStringToBytes(parent_span_id_hex));
    }

    EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U))
        .Times(1)
        .WillRepeatedly(Return(1));
    EXPECT_CALL(
        *mock_client_,
        sendRaw(_, _, Grpc::ProtoBufferEqIgnoreRepeatedFieldOrdering(request_proto), _, _, _));
  }

  // Finishes the span and expects one export request with one span that has the given fields.
  void expectExportedOnFinish(Tracing::Span& span, const std::string& span_fields_yaml,
                              absl::string_view trace_id_hex, absl::string_view span_id_hex,
                              absl::string_view parent_span_id_hex, SystemTime start_time) {
    expectExport(span_fields_yaml, trace_id_hex, span_id_hex, parent_span_id_hex, start_time);
    span.finishSpan();
    EXPECT_EQ(1U, stats_.counter("tracing.opentelemetry.spans_sent").value());
  }

  std::string injectedHeader(Tracing::Span& span, const Tracing::TraceContextHandler& header) {
    Tracing::TestTraceContextImpl upstream_headers{
        {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};
    span.injectContext(upstream_headers, Tracing::UpstreamContext());
    auto value = header.get(upstream_headers);
    // A header that is missing must not look like an empty one.
    return value.has_value() ? std::string(value.value()) : "(not set)";
  }

  std::string injectedTraceparent(Tracing::Span& span) {
    return injectedHeader(span, OpenTelemetryConstants::get().TRACE_PARENT);
  }

  std::string injectedTracestate(Tracing::Span& span) {
    return injectedHeader(span, OpenTelemetryConstants::get().TRACE_STATE);
  }

  // The generic tracer the connection manager uses, on top of the driver under test. It sets the
  // node id and zone tags on every span it starts.
  Tracing::TracerImpl& tracerImpl() {
    if (tracer_impl_ == nullptr) {
      ON_CALL(local_info_, nodeName()).WillByDefault(ReturnRef(node_name_));
      ON_CALL(local_info_, zoneName()).WillByDefault(ReturnRef(zone_name_));
      tracer_impl_ = std::make_unique<Tracing::TracerImpl>(
          Tracing::DriverSharedPtr(std::move(driver_)), local_info_);
    }
    return *tracer_impl_;
  }

protected:
  TestScopedRuntime scoped_runtime_;
  const SystemTime timestamp_{time_system_.systemTime()};
  NiceMock<LocalInfo::MockLocalInfo> local_info_;
  const std::string node_name_{"node_name"};
  const std::string zone_name_{"zone_name"};
  std::unique_ptr<Tracing::TracerImpl> tracer_impl_;
};

INSTANTIATE_TEST_SUITE_P(DeferredSpanGuard, OpenTelemetryDeferredSpanTest, testing::Bool());

// A root span that Envoy decides not to trace carries its ids and propagates them with the
// sampled flag not set.
TEST_P(OpenTelemetryDeferredSpanTest, RootSpanNotTraced) {
  setupValidDriver();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};
  // Trace id high, trace id low and span id, in this order.
  expectRandom({0x0123456789abcdef, 0xfedcba9876543210, 0xff});

  Tracing::SpanPtr span =
      driver_->startSpan(mock_tracing_config_, request_headers, stream_info_, operation_name_,
                         {Tracing::Reason::NotTraceable, false});

  EXPECT_EQ(GetParam(), notBuilt(*span));
  EXPECT_TRUE(span->useLocalDecision());
  EXPECT_FALSE(span->exportedSpan());
  EXPECT_EQ("0123456789abcdeffedcba9876543210", span->getTraceId());
  EXPECT_EQ("00000000000000ff", span->getSpanId());
  EXPECT_EQ("00-0123456789abcdeffedcba9876543210-00000000000000ff-00", injectedTraceparent(*span));
  EXPECT_EQ("", injectedTracestate(*span));

  span->reserveTags(4);
  span->log(SystemTime(), "");
  span->setBaggage("baggage_key", "baggage_value");
  EXPECT_EQ("", span->getBaggage("baggage_key"));
  span->setSampled(false);
  EXPECT_EQ(GetParam(), notBuilt(*span));
  EXPECT_FALSE(span->exportedSpan());

  expectNothingExportedOnFinish(*span);
  EXPECT_EQ(GetParam(), notBuilt(*span));
}

// The path of a real request: the generic tracer starts the span and sets its node id and zone
// tags, the context is injected into a real request header map, and the span is finalized. With
// the guard on no OTLP span is built at any point.
TEST_P(OpenTelemetryDeferredSpanTest, RequestPathBuildsNoSpanWhenNotTraced) {
  setupValidDriver();
  expectRandom({0x0123456789abcdef, 0xfedcba9876543210, 0xff});
  Http::TestRequestHeaderMapImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}, {"x-request-id", "id"}};
  Tracing::HttpTraceContext trace_context(request_headers);

  Tracing::SpanPtr span = tracerImpl().startSpan(mock_tracing_config_, trace_context, stream_info_,
                                                 {Tracing::Reason::NotTraceable, false});
  EXPECT_EQ(GetParam(), deferredSpan(*span));
  EXPECT_EQ(GetParam(), notBuilt(*span));

  // What the router does for the upstream request.
  Http::TestRequestHeaderMapImpl upstream_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};
  Tracing::HttpTraceContext upstream_trace_context(upstream_headers);
  span->injectContext(upstream_trace_context, Tracing::UpstreamContext());
  EXPECT_EQ("00-0123456789abcdeffedcba9876543210-00000000000000ff-00",
            upstream_headers.get_("traceparent"));
  // The tracestate header is set, and empty, as with a recording span.
  ASSERT_FALSE(upstream_headers.get(Http::LowerCaseString("tracestate")).empty());
  EXPECT_EQ("", upstream_headers.get_("tracestate"));
  EXPECT_EQ(GetParam(), notBuilt(*span));

  // What the connection manager does when the request is done.
  Http::TestResponseHeaderMapImpl response_headers{{":status", "200"}};
  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U)).Times(0);
  EXPECT_CALL(*mock_client_, sendRaw(_, _, _, _, _, _)).Times(0);
  Tracing::HttpTracerUtility::finalizeDownstreamSpan(*span, &request_headers, &response_headers,
                                                     nullptr, stream_info_, mock_tracing_config_);
  EXPECT_EQ(0U, stats_.counter("tracing.opentelemetry.spans_sent").value());
  EXPECT_EQ(GetParam(), notBuilt(*span));
}

// The same request path, with the span set to sampled later, as the connection manager does on a
// route refresh. The exported span has the ids that were propagated, the tags the generic tracer
// set before the span was sampled, and the name set before it was sampled.
TEST_P(OpenTelemetryDeferredSpanTest, RequestPathSetToSampledLater) {
  setupValidDriver();
  expectRandom({0x0123456789abcdef, 0xfedcba9876543210, 0xff});
  Http::TestRequestHeaderMapImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};
  Tracing::HttpTraceContext trace_context(request_headers);

  Tracing::SpanPtr span = tracerImpl().startSpan(mock_tracing_config_, trace_context, stream_info_,
                                                 {Tracing::Reason::NotTraceable, false});
  span->setOperation("renamed");
  span->setTypedTag("typed_tag_name", "7", Tracing::TagValueType::Int);
  span->log(SystemTime(SystemTime::duration(Event::TimeSystem::Milliseconds(1))), "event1");
  EXPECT_EQ(GetParam(), notBuilt(*span));
  EXPECT_FALSE(span->exportedSpan());

  EXPECT_TRUE(span->useLocalDecision());
  span->setSampled(true);

  EXPECT_EQ(GetParam(), deferredSpan(*span));
  EXPECT_FALSE(notBuilt(*span));
  EXPECT_TRUE(span->exportedSpan());
  EXPECT_TRUE(span->useLocalDecision());
  EXPECT_EQ("0123456789abcdeffedcba9876543210", span->getTraceId());
  EXPECT_EQ("00000000000000ff", span->getSpanId());
  Http::TestRequestHeaderMapImpl upstream_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};
  Tracing::HttpTraceContext upstream_trace_context(upstream_headers);
  span->injectContext(upstream_trace_context, Tracing::UpstreamContext());
  EXPECT_EQ("00-0123456789abcdeffedcba9876543210-00000000000000ff-01",
            upstream_headers.get_("traceparent"));

  // Set after the span was sampled: it follows the earlier tags.
  span->setTag("late_tag_name", "late_tag_value");

  const std::string span_fields = R"(
      name: "renamed"
      kind: SPAN_KIND_SERVER
      flags: 257
      attributes:
        - key: "node_id"
          value:
            string_value: "node_name"
        - key: "zone"
          value:
            string_value: "zone_name"
        - key: "typed_tag_name"
          value:
            int_value: 7
        - key: "late_tag_name"
          value:
            string_value: "late_tag_value"
      events:
        - name: event1
          time_unix_nano: 1000000)";
  expectExportedOnFinish(*span, span_fields, "0123456789abcdeffedcba9876543210", "00000000000000ff",
                         "", timestamp_);
}

// The repeated fields of the exported span keep the order in which they were set, also for what
// was set before the span was sampled.
TEST_P(OpenTelemetryDeferredSpanTest, RecordsKeepTheirOrder) {
  setupValidDriver();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};

  Tracing::SpanPtr span =
      driver_->startSpan(mock_tracing_config_, request_headers, stream_info_, operation_name_,
                         {Tracing::Reason::NotTraceable, false});
  span->setTag("a", "1");
  span->setTypedTag("b", "true", Tracing::TagValueType::Bool);
  span->setTag("", "ignored");
  span->setTag("a", "2");
  span->setTag(Tracing::Tags::get().HttpStatusCode, "503");
  span->log(SystemTime(SystemTime::duration(Event::TimeSystem::Milliseconds(2))), "event2");
  span->log(SystemTime(SystemTime::duration(Event::TimeSystem::Milliseconds(1))), "event1");
  span->setSampled(true);
  span->setTag("c", "3");

  const auto* otlp_span = otlpSpan(*span);
  ASSERT_NE(nullptr, otlp_span);
  ASSERT_EQ(4, otlp_span->attributes_size());
  EXPECT_EQ("a", otlp_span->attributes(0).key());
  EXPECT_EQ("2", otlp_span->attributes(0).value().string_value());
  EXPECT_EQ("b", otlp_span->attributes(1).key());
  EXPECT_TRUE(otlp_span->attributes(1).value().bool_value());
  EXPECT_EQ(Tracing::Tags::get().HttpStatusCode, otlp_span->attributes(2).key());
  EXPECT_EQ("c", otlp_span->attributes(3).key());
  EXPECT_EQ(::opentelemetry::proto::trace::v1::Status::STATUS_CODE_ERROR,
            otlp_span->status().code());
  ASSERT_EQ(2, otlp_span->events_size());
  EXPECT_EQ("event2", otlp_span->events(0).name());
  EXPECT_EQ("event1", otlp_span->events(1).name());
}

// A span whose remote parent is not sampled keeps the parent's trace id and tracestate.
TEST_P(OpenTelemetryDeferredSpanTest, ParentNotSampled) {
  setupValidDriver();
  // The trace id is in upper case: it is propagated in lower case.
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"},
      {":path", "/"},
      {":method", "GET"},
      {"traceparent", "00-0AF7651916CD43DD8448EB211C80319C-b7ad6b7169203331-00"},
      {"tracestate", "congo=t61rcWkgMzE"}};
  // Only the span id is generated.
  expectRandom({0x00f067aa0ba902b7});

  Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                             operation_name_, {Tracing::Reason::Sampling, true});

  EXPECT_EQ(GetParam(), notBuilt(*span));
  EXPECT_FALSE(span->useLocalDecision());
  EXPECT_FALSE(span->exportedSpan());
  EXPECT_EQ("0af7651916cd43dd8448eb211c80319c", span->getTraceId());
  EXPECT_EQ("00f067aa0ba902b7", span->getSpanId());
  EXPECT_EQ("00-0af7651916cd43dd8448eb211c80319c-00f067aa0ba902b7-00", injectedTraceparent(*span));
  EXPECT_EQ("congo=t61rcWkgMzE", injectedTracestate(*span));

  expectNothingExportedOnFinish(*span);
}

// The Envoy tracing decision vetoes a sampled parent: the span is not sampled either.
TEST_P(OpenTelemetryDeferredSpanTest, SampledParentNotTracedByEnvoy) {
  setupValidDriver();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"},
      {":path", "/"},
      {":method", "GET"},
      {"traceparent", "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01"},
      {"tracestate", "congo=t61rcWkgMzE"}};
  expectRandom({0x00f067aa0ba902b7});

  Tracing::SpanPtr span =
      driver_->startSpan(mock_tracing_config_, request_headers, stream_info_, operation_name_,
                         {Tracing::Reason::NotTraceable, false});

  EXPECT_EQ(GetParam(), notBuilt(*span));
  EXPECT_FALSE(span->useLocalDecision());
  EXPECT_FALSE(span->exportedSpan());
  EXPECT_EQ("00-0af7651916cd43dd8448eb211c80319c-00f067aa0ba902b7-00", injectedTraceparent(*span));
  EXPECT_EQ("congo=t61rcWkgMzE", injectedTracestate(*span));

  expectNothingExportedOnFinish(*span);
}

// A sampled parent that Envoy traces takes a recording span, whatever the guard says.
TEST_P(OpenTelemetryDeferredSpanTest, SampledParentIsRecordingSpan) {
  setupValidDriver();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"},
      {":path", "/"},
      {":method", "GET"},
      {"traceparent", "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01"}};
  expectRandom({0x00f067aa0ba902b7});

  Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                             operation_name_, {Tracing::Reason::Sampling, true});

  EXPECT_FALSE(deferredSpan(*span));
  EXPECT_TRUE(span->exportedSpan());
  EXPECT_EQ("00-0af7651916cd43dd8448eb211c80319c-00f067aa0ba902b7-01", injectedTraceparent(*span));
}

// With a sampler configured the sampler decides, so the span is a recording span even when Envoy
// does not trace the request.
TEST_P(OpenTelemetryDeferredSpanTest, SamplerConfiguredIsRecordingSpan) {
  setupValidDriverWithAlwaysOnSampler();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};

  Tracing::SpanPtr span =
      driver_->startSpan(mock_tracing_config_, request_headers, stream_info_, operation_name_,
                         {Tracing::Reason::NotTraceable, false});

  EXPECT_FALSE(deferredSpan(*span));
  EXPECT_FALSE(span->exportedSpan());
  expectNothingExportedOnFinish(*span);
}

// What is set on a span with a remote parent while it is not sampled is exported when the span is
// set to sampled later. The parent span id, the remote-parent flag and the tracestate are kept.
TEST_P(OpenTelemetryDeferredSpanTest, ParentNotSampledSetToSampledLater) {
  setupValidDriver();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"},
      {":path", "/"},
      {":method", "GET"},
      {"traceparent", "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-00"},
      {"tracestate", "congo=t61rcWkgMzE"}};
  expectRandom({0x00f067aa0ba902b7});

  Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                             operation_name_, {Tracing::Reason::Sampling, true});

  span->setTag("first_tag_name", "first_tag_value");
  span->log(SystemTime(SystemTime::duration(Event::TimeSystem::Milliseconds(1))), "event1");

  // Setting a tag or an event does not make the span sampled, and builds nothing.
  EXPECT_EQ(GetParam(), notBuilt(*span));
  EXPECT_FALSE(span->exportedSpan());
  EXPECT_FALSE(span->useLocalDecision());
  EXPECT_EQ("00-0af7651916cd43dd8448eb211c80319c-00f067aa0ba902b7-00", injectedTraceparent(*span));

  span->setSampled(true);
  EXPECT_TRUE(span->exportedSpan());
  EXPECT_FALSE(span->useLocalDecision());
  EXPECT_EQ("0af7651916cd43dd8448eb211c80319c", span->getTraceId());
  EXPECT_EQ("00f067aa0ba902b7", span->getSpanId());
  EXPECT_EQ("00-0af7651916cd43dd8448eb211c80319c-00f067aa0ba902b7-01", injectedTraceparent(*span));
  EXPECT_EQ("congo=t61rcWkgMzE", injectedTracestate(*span));

  const std::string span_fields = R"(
      name: "test"
      kind: SPAN_KIND_SERVER
      trace_state: "congo=t61rcWkgMzE"
      flags: 769
      attributes:
        - key: "first_tag_name"
          value:
            string_value: "first_tag_value"
      events:
        - name: event1
          time_unix_nano: 1000000)";
  expectExportedOnFinish(*span, span_fields, "0af7651916cd43dd8448eb211c80319c", "00f067aa0ba902b7",
                         "b7ad6b7169203331", timestamp_);
}

// A span that was given tags and stayed not sampled is not exported.
TEST_P(OpenTelemetryDeferredSpanTest, TaggedAndNeverSampledIsNotExported) {
  setupValidDriver();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};

  Tracing::SpanPtr span =
      driver_->startSpan(mock_tracing_config_, request_headers, stream_info_, operation_name_,
                         {Tracing::Reason::NotTraceable, false});
  span->setTag("first_tag_name", "first_tag_value");
  span->reserveTags(2);
  span->setOperation("renamed");
  EXPECT_FALSE(span->exportedSpan());

  // The local decision can still be switched off.
  EXPECT_TRUE(span->useLocalDecision());
  span->disableLocalDecision();
  EXPECT_FALSE(span->useLocalDecision());

  expectNothingExportedOnFinish(*span);
}

// Switching off the local decision before the span is sampled.
TEST_P(OpenTelemetryDeferredSpanTest, DisableLocalDecisionBeforeSampled) {
  setupValidDriver();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};

  Tracing::SpanPtr span =
      driver_->startSpan(mock_tracing_config_, request_headers, stream_info_, operation_name_,
                         {Tracing::Reason::NotTraceable, false});
  EXPECT_TRUE(span->useLocalDecision());
  span->disableLocalDecision();
  EXPECT_FALSE(span->useLocalDecision());
  EXPECT_EQ(GetParam(), notBuilt(*span));

  // The setting is kept when the span is sampled, and the span can be set back to not sampled.
  span->setSampled(true);
  EXPECT_FALSE(span->useLocalDecision());
  EXPECT_TRUE(span->exportedSpan());
  span->setSampled(false);
  EXPECT_FALSE(span->exportedSpan());
  expectNothingExportedOnFinish(*span);
}

// The child of a span that is not sampled is not sampled, shares the trace id and tracestate, and
// has the span as its parent when it is set to sampled later.
TEST_P(OpenTelemetryDeferredSpanTest, ChildOfSpanNotSampled) {
  setupValidDriver();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"},
      {":path", "/"},
      {":method", "GET"},
      {"traceparent", "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-00"},
      {"tracestate", "congo=t61rcWkgMzE"}};
  // The span id of the span, then the span id of its child.
  expectRandom({0x00f067aa0ba902b7, 0x4});

  Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                             operation_name_, {Tracing::Reason::Sampling, true});
  const SystemTime child_start_time =
      timestamp_ + SystemTime::duration(Event::TimeSystem::Milliseconds(5));
  Tracing::SpanPtr child_span = span->spawnChild(mock_tracing_config_, "child", child_start_time);
  // What the router sets on an upstream span.
  child_span->setTag(Tracing::Tags::get().UpstreamCluster, "cluster");

  // Neither span has built anything.
  EXPECT_EQ(GetParam(), notBuilt(*span));
  EXPECT_EQ(GetParam(), notBuilt(*child_span));
  EXPECT_FALSE(child_span->useLocalDecision());
  EXPECT_FALSE(child_span->exportedSpan());
  EXPECT_EQ("0af7651916cd43dd8448eb211c80319c", child_span->getTraceId());
  EXPECT_EQ("0000000000000004", child_span->getSpanId());
  EXPECT_EQ("00-0af7651916cd43dd8448eb211c80319c-0000000000000004-00",
            injectedTraceparent(*child_span));
  EXPECT_EQ("congo=t61rcWkgMzE", injectedTracestate(*child_span));

  child_span->setSampled(true);
  const std::string span_fields = R"(
      name: "child"
      kind: SPAN_KIND_CLIENT
      trace_state: "congo=t61rcWkgMzE"
      flags: 257
      attributes:
        - key: "upstream_cluster"
          value:
            string_value: "cluster")";
  expectExportedOnFinish(*child_span, span_fields, "0af7651916cd43dd8448eb211c80319c",
                         "0000000000000004", "00f067aa0ba902b7", child_start_time);
}

// A child spawned after its parent was set to sampled is a sampled recording span.
TEST_P(OpenTelemetryDeferredSpanTest, ChildOfSpanSetToSampledLater) {
  setupValidDriver();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};
  expectRandom({0x0123456789abcdef, 0xfedcba9876543210, 0xff, 0x4});

  Tracing::SpanPtr span =
      driver_->startSpan(mock_tracing_config_, request_headers, stream_info_, operation_name_,
                         {Tracing::Reason::NotTraceable, false});
  span->setSampled(true);
  Tracing::SpanPtr child_span = span->spawnChild(mock_tracing_config_, "child", timestamp_);

  EXPECT_FALSE(deferredSpan(*child_span));
  EXPECT_TRUE(child_span->exportedSpan());
  EXPECT_EQ("00-0123456789abcdeffedcba9876543210-0000000000000004-01",
            injectedTraceparent(*child_span));
  const std::string span_fields = R"(
      name: "child"
      kind: SPAN_KIND_CLIENT
      flags: 257)";
  expectExportedOnFinish(*child_span, span_fields, "0123456789abcdeffedcba9876543210",
                         "0000000000000004", "00000000000000ff", timestamp_);
}

// The guard is read for each span: a change applies to the next span of the same tracer.
TEST_P(OpenTelemetryDeferredSpanTest, GuardChangeAppliesToTheNextSpan) {
  setupValidDriver();
  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};

  Tracing::SpanPtr first_span =
      driver_->startSpan(mock_tracing_config_, request_headers, stream_info_, operation_name_,
                         {Tracing::Reason::NotTraceable, false});
  setGuard(!GetParam());
  Tracing::SpanPtr second_span =
      driver_->startSpan(mock_tracing_config_, request_headers, stream_info_, operation_name_,
                         {Tracing::Reason::NotTraceable, false});

  EXPECT_EQ(GetParam(), deferredSpan(*first_span));
  EXPECT_EQ(!GetParam(), deferredSpan(*second_span));
}

// The tracer itself, without the driver in front of it, for parent contexts the driver's header
// parsing never produces.
class OpenTelemetryTracerDeferredSpanTest : public testing::TestWithParam<bool> {
public:
  OpenTelemetryTracerDeferredSpanTest()
      : stats_{OPENTELEMETRY_TRACER_STATS(
            POOL_COUNTER_PREFIX(*store_.rootScope(), "tracing.opentelemetry."))} {
    scoped_runtime_.mergeValues(
        {{"envoy.reloadable_features.otel_tracer_deferred_span", GetParam() ? "true" : "false"}});
    tracer_ = std::make_unique<Tracer>(nullptr, time_system_, random_, runtime_, dispatcher_,
                                       stats_, std::make_shared<Resource>(), nullptr, 16);
  }

  Tracing::SpanPtr startSpan(const SpanContext& parent_context) {
    return tracer_->startSpan("test", stream_info_, time_system_.systemTime(), parent_context,
                              {Tracing::Reason::Sampling, true}, {},
                              ::opentelemetry::proto::trace::v1::Span::SPAN_KIND_SERVER);
  }

protected:
  TestScopedRuntime scoped_runtime_;
  Event::SimulatedTimeSystem time_system_;
  NiceMock<Random::MockRandomGenerator> random_;
  NiceMock<Runtime::MockLoader> runtime_;
  NiceMock<Event::MockDispatcher> dispatcher_;
  NiceMock<Stats::MockIsolatedStatsStore> store_;
  OpenTelemetryTracerStats stats_;
  NiceMock<StreamInfo::MockStreamInfo> stream_info_;
  TracerPtr tracer_;
};

INSTANTIATE_TEST_SUITE_P(DeferredSpanGuard, OpenTelemetryTracerDeferredSpanTest, testing::Bool());

// A parent context without a span id: the span has no parent span id, before and after it is
// sampled.
TEST_P(OpenTelemetryTracerDeferredSpanTest, ParentWithoutSpanId) {
  EXPECT_CALL(random_, random()).WillOnce(Return(0x4));
  const SpanContext parent_context("00", "0af7651916cd43dd8448eb211c80319c", "", false,
                                   "congo=t61rcWkgMzE", true);

  Tracing::SpanPtr span = startSpan(parent_context);

  EXPECT_EQ(GetParam(), OpenTelemetryDeferredSpanTest::notBuilt(*span));
  EXPECT_EQ("0af7651916cd43dd8448eb211c80319c", span->getTraceId());
  EXPECT_EQ("0000000000000004", span->getSpanId());

  span->setSampled(true);
  const auto* otlp_span = OpenTelemetryDeferredSpanTest::otlpSpan(*span);
  ASSERT_NE(nullptr, otlp_span);
  EXPECT_EQ(absl::HexStringToBytes("0af7651916cd43dd8448eb211c80319c"), otlp_span->trace_id());
  EXPECT_EQ(absl::HexStringToBytes("0000000000000004"), otlp_span->span_id());
  EXPECT_TRUE(otlp_span->parent_span_id().empty());
  EXPECT_EQ("congo=t61rcWkgMzE", otlp_span->trace_state());
}

// Ids that are not 32 and 16 hex digits take a recording span from the start, with the guard on
// and off, and the ids come out as they always did.
TEST_P(OpenTelemetryTracerDeferredSpanTest, MalformedParentIdsTakeRecordingSpan) {
  for (const auto& [trace_id, span_id] : std::vector<std::pair<std::string, std::string>>{
           {"0af7651916cd43dd8448eb211c8031", "b7ad6b7169203331"},
           {"0af7651916cd43dd8448eb211c80319c", "b7ad6b71692033"},
           {"0xf7651916cd43dd8448eb211c80319c", "b7ad6b7169203331"},
           {"0af7651916cd43dd8448eb211c80319c", "+7ad6b7169203331"}}) {
    const SpanContext parent_context("00", trace_id, span_id, false, "", true);
    Tracing::SpanPtr span = startSpan(parent_context);
    EXPECT_FALSE(OpenTelemetryDeferredSpanTest::deferredSpan(*span)) << trace_id << " " << span_id;
  }
}

// Verifies tracer throws exception when no exporter is configured
TEST_F(OpenTelemetryDriverTest, NoExporterConfigured) {
  const std::string yaml_string = "{}";
  envoy::config::trace::v3::OpenTelemetryConfig opentelemetry_config;
  TestUtility::loadFromYaml(yaml_string, opentelemetry_config);
  EXPECT_THROW_WITH_MESSAGE(setup(opentelemetry_config), EnvoyException,
                            "OpenTelemetry Tracer must have exactly one of gRPC, HTTP, or custom "
                            "exporter configured.");
  EXPECT_EQ(driver_, nullptr);
}

// Verifies a custom service name is properly set on exported spans
TEST_F(OpenTelemetryDriverTest, ExportSpanWithCustomServiceName) {
  const std::string yaml_string = R"EOF(
    grpc_service:
      envoy_grpc:
        cluster_name: fake-cluster
      timeout: 0.250s
    service_name: test-service-name
    )EOF";
  envoy::config::trace::v3::OpenTelemetryConfig opentelemetry_config;
  TestUtility::loadFromYaml(yaml_string, opentelemetry_config);
  setup(opentelemetry_config);

  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};
  NiceMock<Random::MockRandomGenerator>& mock_random_generator_ =
      context_.server_factory_context_.api_.random_;
  int64_t generated_int = 1;
  EXPECT_CALL(mock_random_generator_, random()).Times(3).WillRepeatedly(Return(generated_int));
  SystemTime timestamp = time_system_.systemTime();
  ON_CALL(stream_info_, startTime()).WillByDefault(Return(timestamp));

  Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                             operation_name_, {Tracing::Reason::Sampling, true});
  EXPECT_NE(span.get(), nullptr);

  constexpr absl::string_view request_yaml = R"(
resource_spans:
  resource:
    attributes:
      key: "service.name"
      value:
        string_value: "test-service-name"
      key: "key1"
      value:
        string_value: "val1"
  scope_spans:
    scope:
      name: "envoy"
      version: {}
    spans:
      trace_id: "AAA"
      span_id: "AAA"
      name: "test"
      kind: SPAN_KIND_SERVER
      start_time_unix_nano: {}
      end_time_unix_nano: {}
      flags: 257
  )";
  opentelemetry::proto::collector::trace::v1::ExportTraceServiceRequest request_proto;
  int64_t timestamp_ns = std::chrono::nanoseconds(timestamp.time_since_epoch()).count();
  absl::string_view envoy_version = Envoy::VersionInfo::version();

  TestUtility::loadFromYaml(fmt::format(request_yaml, envoy_version, timestamp_ns, timestamp_ns),
                            request_proto);
  std::string generated_int_hex = Hex::uint64ToHex(generated_int);
  auto* expected_span =
      request_proto.mutable_resource_spans(0)->mutable_scope_spans(0)->mutable_spans(0);
  expected_span->set_trace_id(
      absl::HexStringToBytes(absl::StrCat(generated_int_hex, generated_int_hex)));
  expected_span->set_span_id(absl::HexStringToBytes(absl::StrCat(generated_int_hex)));

  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U))
      .Times(1)
      .WillRepeatedly(Return(1));
  EXPECT_CALL(
      *mock_client_,
      sendRaw(_, _, Grpc::ProtoBufferEqIgnoreRepeatedFieldOrdering(request_proto), _, _, _));
  span->finishSpan();
  EXPECT_EQ(1U, stats_.counter("tracing.opentelemetry.spans_sent").value());
}

// Verifies the export using the HTTP exporter
TEST_F(OpenTelemetryDriverTest, ExportOTLPSpanHTTP) {
  context_.server_factory_context_.cluster_manager_.thread_local_cluster_.cluster_.info_->name_ =
      "my_o11y_backend";
  context_.server_factory_context_.cluster_manager_.initializeThreadLocalClusters(
      {"my_o11y_backend"});
  ON_CALL(context_.server_factory_context_.cluster_manager_.thread_local_cluster_,
          httpAsyncClient())
      .WillByDefault(ReturnRef(
          context_.server_factory_context_.cluster_manager_.thread_local_cluster_.async_client_));
  context_.server_factory_context_.cluster_manager_.initializeClusters({"my_o11y_backend"}, {});
  setupValidDriverWithHttpExporter();

  Tracing::TestTraceContextImpl request_headers{
      {":authority", "test.com"}, {":path", "/"}, {":method", "GET"}};
  Tracing::SpanPtr span = driver_->startSpan(mock_tracing_config_, request_headers, stream_info_,
                                             operation_name_, {Tracing::Reason::Sampling, true});
  EXPECT_NE(span.get(), nullptr);

  // Check that spans are buffered and flushed properly with max cache size.
  // Flush after a single span.
  EXPECT_CALL(runtime_.snapshot_, getInteger("tracing.opentelemetry.min_flush_spans", 5U))
      .Times(1)
      .WillRepeatedly(Return(1));
  // We should see a call to the async client to export that single span.
  EXPECT_CALL(context_.server_factory_context_.cluster_manager_.thread_local_cluster_.async_client_,
              send_(_, _, _));

  span->finishSpan();

  EXPECT_EQ(1U, stats_.counter("tracing.opentelemetry.spans_sent").value());
}

} // namespace OpenTelemetry
} // namespace Tracers
} // namespace Extensions
} // namespace Envoy
