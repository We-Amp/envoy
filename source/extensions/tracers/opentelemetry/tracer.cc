#include "source/extensions/tracers/opentelemetry/tracer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

#include "envoy/config/trace/v3/opentelemetry.pb.h"

#include "source/common/common/assert.h"
#include "source/common/common/empty_string.h"
#include "source/common/common/hex.h"
#include "source/common/runtime/runtime_features.h"
#include "source/common/tracing/common_values.h"
#include "source/common/tracing/trace_context_impl.h"
#include "source/common/version/version.h"
#include "source/extensions/tracers/opentelemetry/otlp_utils.h"

#include "absl/strings/numbers.h"
#include "opentelemetry/proto/collector/trace/v1/trace_service.pb.h"
#include "opentelemetry/proto/trace/v1/trace.pb.h"

namespace Envoy {
namespace Extensions {
namespace Tracers {
namespace OpenTelemetry {

constexpr absl::string_view kDefaultVersion = "00";
// The sampled bit of the W3C trace flags carried in the low 8 bits of the OTLP span flags.
// See https://www.w3.org/TR/trace-context/#sampled-flag.
constexpr uint32_t TraceFlagsSampledMask = 0x1;

using opentelemetry::proto::collector::trace::v1::ExportTraceServiceRequest;

namespace {

const Tracing::TraceContextHandler& traceParentHeader() {
  CONSTRUCT_ON_FIRST_USE(Tracing::TraceContextHandler, "traceparent");
}

const Tracing::TraceContextHandler& traceStateHeader() {
  CONSTRUCT_ON_FIRST_USE(Tracing::TraceContextHandler, "tracestate");
}

constexpr absl::string_view kHexDigits = "0123456789abcdef";

// Writes the lowercase hex form of `bytes` to `out`, which must have room for twice its size.
template <size_t N> void writeHex(const std::array<uint8_t, N>& bytes, char* out) {
  for (const uint8_t byte : bytes) {
    *out++ = kHexDigits[byte >> 4];
    *out++ = kHexDigits[byte & 0xf];
  }
}

template <size_t N> std::string toHex(const std::array<uint8_t, N>& bytes) {
  std::string hex(2 * N, '0');
  writeHex(bytes, hex.data());
  return hex;
}

int hexDigitValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// Reads exactly 2 * N hex digits. Returns false, with `bytes` unspecified, for any other input.
template <size_t N> bool fromHex(absl::string_view hex, std::array<uint8_t, N>& bytes) {
  if (hex.size() != 2 * N) {
    return false;
  }
  for (size_t i = 0; i < N; i++) {
    const int high = hexDigitValue(hex[2 * i]);
    const int low = hexDigitValue(hex[2 * i + 1]);
    if (high < 0 || low < 0) {
      return false;
    }
    bytes[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

// Stores `value` at `out` in big-endian order, the byte order Hex::uint64ToHex() prints.
void storeBigEndian(uint64_t value, uint8_t* out) {
  for (int i = 7; i >= 0; i--) {
    out[i] = static_cast<uint8_t>(value & 0xff);
    value >>= 8;
  }
}

void callSampler(SamplerSharedPtr sampler, const StreamInfo::StreamInfo& stream_info,
                 const std::optional<SpanContext> span_context, Span& new_span,
                 const std::string& operation_name,
                 OptRef<const Tracing::TraceContext> trace_context) {
  if (!sampler) {
    return;
  }
  const auto sampling_result =
      sampler->shouldSample(stream_info, span_context, new_span.getTraceId(), operation_name,
                            new_span.spankind(), trace_context, {});
  new_span.setSampled(sampling_result.isSampled());

  if (sampling_result.attributes) {
    for (auto const& attribute : *sampling_result.attributes) {
      new_span.setAttribute(attribute.first, attribute.second);
    }
  }
  if (!sampling_result.tracestate.empty()) {
    new_span.setTracestate(sampling_result.tracestate);
  }
}

} // namespace

Span::Span(const std::string& name, const StreamInfo::StreamInfo& stream_info,
           SystemTime start_time, Envoy::TimeSource& time_source, Tracer& parent_tracer,
           OTelSpanKind span_kind, bool use_local_decision)
    : stream_info_(stream_info), parent_tracer_(parent_tracer), time_source_(time_source),
      use_local_decision_(use_local_decision) {
  span_ = ::opentelemetry::proto::trace::v1::Span();

  span_.set_kind(span_kind);

  span_.set_name(name);
  span_.set_start_time_unix_nano(std::chrono::nanoseconds(start_time.time_since_epoch()).count());
}

Tracing::SpanPtr Span::spawnChild(const Tracing::Config&, const std::string& name,
                                  SystemTime start_time) {
  // Build span_context from the current span, then generate the child span from that context.
  SpanContext span_context(kDefaultVersion, getTraceId(), spanId(), sampled(),
                           std::string(tracestate()), /*is_remote=*/false);
  return parent_tracer_.startSpan(name, stream_info_, start_time, span_context, {},
                                  ::opentelemetry::proto::trace::v1::Span::SPAN_KIND_CLIENT);
}

void Span::finishSpan() {
  // Call into the parent tracer so we can access the shared exporter.
  span_.set_end_time_unix_nano(
      std::chrono::nanoseconds(time_source_.systemTime().time_since_epoch()).count());
  // Bits 0-7 of the span flags carry the W3C trace flags; bits 8 and 9 encode the tri-state
  // "is the parent context remote" (unknown/local/remote). The parent's remoteness is always
  // known here, so the HAS_IS_REMOTE bit is set unconditionally.
  uint32_t flags = static_cast<uint32_t>(
      ::opentelemetry::proto::trace::v1::SPAN_FLAGS_CONTEXT_HAS_IS_REMOTE_MASK);
  if (parent_context_is_remote_) {
    flags |=
        static_cast<uint32_t>(::opentelemetry::proto::trace::v1::SPAN_FLAGS_CONTEXT_IS_REMOTE_MASK);
  }
  if (sampled()) {
    flags |= TraceFlagsSampledMask;
  }
  span_.set_flags(flags);
  if (sampled()) {
    parent_tracer_.sendSpan(span_);
  }
}

void Span::setOperation(absl::string_view operation) { span_.set_name(operation); };

void Span::injectContext(Tracing::TraceContext& trace_context, const Tracing::UpstreamContext&) {
  std::string trace_id_hex = absl::BytesToHexString(span_.trace_id());
  std::string span_id_hex = absl::BytesToHexString(span_.span_id());
  std::vector<uint8_t> trace_flags_vec{sampled()};
  std::string trace_flags_hex = Hex::encode(trace_flags_vec);
  std::string traceparent_header_value =
      absl::StrCat(kDefaultVersion, "-", trace_id_hex, "-", span_id_hex, "-", trace_flags_hex);
  // Set the traceparent in the trace_context.
  traceParentHeader().setRefKey(trace_context, traceparent_header_value);
  // Also set the tracestate.
  traceStateHeader().setRefKey(trace_context, span_.trace_state());
}

void Span::setAttribute(absl::string_view name, const OTelAttribute& attribute_value) {
  // The attribute key MUST be a non-null and non-empty string.
  if (name.empty()) {
    return;
  }
  // Attribute keys MUST be unique.
  // If a value already exists for this key, overwrite it.
  for (auto& key_value : *span_.mutable_attributes()) {
    if (key_value.key() == name) {
      OtlpUtils::populateAnyValue(*key_value.mutable_value(), attribute_value);
      return;
    }
  }
  // If we haven't found an existing match already, we can add a new key/value.
  opentelemetry::proto::common::v1::KeyValue key_value =
      opentelemetry::proto::common::v1::KeyValue();
  opentelemetry::proto::common::v1::AnyValue value_proto =
      opentelemetry::proto::common::v1::AnyValue();
  OtlpUtils::populateAnyValue(value_proto, attribute_value);
  key_value.set_key(std::string{name});
  *key_value.mutable_value() = value_proto;
  *span_.add_attributes() = key_value;
}

::opentelemetry::proto::trace::v1::Status_StatusCode
convertGrpcStatusToTraceStatusCode(::opentelemetry::proto::trace::v1::Span_SpanKind kind,
                                   absl::string_view value) {
  uint64_t grpc_status_code;
  if (!absl::SimpleAtoi(value, &grpc_status_code)) {
    // If the value is not a number, we can't map it to a status code.
    // In this case, we should leave the status code unset.
    return ::opentelemetry::proto::trace::v1::Status::STATUS_CODE_UNSET;
  }

  Grpc::Status::GrpcStatus grpc_status = static_cast<Grpc::Status::GrpcStatus>(grpc_status_code);
  // Check mapping https://opentelemetry.io/docs/specs/semconv/rpc/grpc/#grpc-status
  if (kind == ::opentelemetry::proto::trace::v1::Span::SPAN_KIND_CLIENT) {
    if (grpc_status == Grpc::Status::WellKnownGrpcStatus::Ok) {
      return ::opentelemetry::proto::trace::v1::Status::STATUS_CODE_UNSET;
    }
    return ::opentelemetry::proto::trace::v1::Status::STATUS_CODE_ERROR;
  }

  // SPAN_KIND_SERVER
  switch (grpc_status) {
  case Grpc::Status::WellKnownGrpcStatus::Unknown:
  case Grpc::Status::WellKnownGrpcStatus::DeadlineExceeded:
  case Grpc::Status::WellKnownGrpcStatus::Unimplemented:
  case Grpc::Status::WellKnownGrpcStatus::Internal:
  case Grpc::Status::WellKnownGrpcStatus::Unavailable:
  case Grpc::Status::WellKnownGrpcStatus::DataLoss:
    return ::opentelemetry::proto::trace::v1::Status::STATUS_CODE_ERROR;
  default:
    return ::opentelemetry::proto::trace::v1::Status::STATUS_CODE_UNSET;
  }
}

void Span::setTag(absl::string_view name, absl::string_view value) {
  if (name == Tracing::Tags::get().HttpMethod) {
    static constexpr std::array<absl::string_view, 10> KnownMethods{
        "CONNECT", "DELETE", "GET", "HEAD", "OPTIONS", "PATCH", "POST", "PUT", "QUERY", "TRACE"};
    if (std::find(KnownMethods.begin(), KnownMethods.end(), value) == KnownMethods.end()) {
      value = "_OTHER";
    }
  }
  if (name == Tracing::Tags::get().GrpcStatusCode) {
    span_.mutable_status()->set_code(convertGrpcStatusToTraceStatusCode(span_.kind(), value));
  } else if (name == Tracing::Tags::get().HttpStatusCode) {
    uint64_t status_code;
    // For HTTP status codes in the 5xx range, as well as any other code the client failed to
    // interpret, span status MUST be set to Error.
    //
    // For HTTP status codes in the 4xx range span status MUST be left unset in case of
    // SpanKind.SERVER and MUST be set to Error in case of SpanKind.CLIENT.
    if (absl::SimpleAtoi(value, &status_code)) {
      if (status_code >= 500 ||
          (status_code >= 400 &&
           span_.kind() == ::opentelemetry::proto::trace::v1::Span::SPAN_KIND_CLIENT)) {
        span_.mutable_status()->set_code(
            ::opentelemetry::proto::trace::v1::Status::STATUS_CODE_ERROR);
      }
    }
  }
  setAttribute(name, value);
}

void Span::setTypedTag(absl::string_view name, absl::string_view value,
                       Tracing::TagValueType type) {
  switch (type) {
  case Tracing::TagValueType::Int: {
    int64_t int_value = 0;
    if (absl::SimpleAtoi(value, &int_value)) {
      setAttribute(name, int_value);
      return;
    }
    break;
  }
  case Tracing::TagValueType::Double: {
    double double_value = 0;
    if (absl::SimpleAtod(value, &double_value)) {
      setAttribute(name, double_value);
      return;
    }
    break;
  }
  case Tracing::TagValueType::Bool: {
    bool bool_value = false;
    if (absl::SimpleAtob(value, &bool_value)) {
      setAttribute(name, bool_value);
      return;
    }
    break;
  }
  case Tracing::TagValueType::String:
    break;
  }
  setTag(name, value);
}

void Span::reserveTags(size_t size) {
  auto* attributes = span_.mutable_attributes();
  attributes->Reserve(attributes->size() + size);
}

void Span::log(SystemTime timestamp, const std::string& event) {
  if (event.empty()) {
    return;
  }

  ::opentelemetry::proto::trace::v1::Span::Event span_event =
      ::opentelemetry::proto::trace::v1::Span::Event();
  span_event.set_time_unix_nano(std::chrono::nanoseconds(timestamp.time_since_epoch()).count());
  span_event.set_name(std::string{event});

  *span_.add_events() = span_event;
}

NonRecordingSpan::NonRecordingSpan(const std::string& name,
                                   const StreamInfo::StreamInfo& stream_info, SystemTime start_time,
                                   Envoy::TimeSource& time_source, Tracer& parent_tracer,
                                   OTelSpanKind span_kind, bool use_local_decision,
                                   const TraceId& trace_id, const SpanId& span_id)
    : stream_info_(stream_info), parent_tracer_(parent_tracer), time_source_(time_source),
      start_time_(start_time), span_kind_(span_kind), trace_id_(trace_id), span_id_(span_id),
      name_(name), use_local_decision_(use_local_decision) {}

OpenTelemetry::Span& NonRecordingSpan::recordingSpan() {
  if (recording_span_ == nullptr) {
    recording_span_ =
        std::make_unique<OpenTelemetry::Span>(name_, stream_info_, start_time_, time_source_,
                                              parent_tracer_, span_kind_, use_local_decision_);
    recording_span_->setTraceId(toHex(trace_id_));
    recording_span_->setId(toHex(span_id_));
    if (has_parent_span_id_) {
      recording_span_->setParentId(toHex(parent_span_id_));
    }
    recording_span_->setParentContextIsRemote(parent_context_is_remote_);
    if (!tracestate_.empty()) {
      recording_span_->setTracestate(tracestate_);
    }
    recording_span_->setSampled(false);
  }
  return *recording_span_;
}

void NonRecordingSpan::setOperation(absl::string_view operation) {
  if (recording_span_ != nullptr) {
    recording_span_->setOperation(operation);
    return;
  }
  name_ = std::string{operation};
}

void NonRecordingSpan::setTag(absl::string_view name, absl::string_view value) {
  recordingSpan().setTag(name, value);
}

void NonRecordingSpan::setTypedTag(absl::string_view name, absl::string_view value,
                                   Tracing::TagValueType type) {
  recordingSpan().setTypedTag(name, value, type);
}

void NonRecordingSpan::reserveTags(size_t size) {
  // Only a capacity hint: there is nothing to reserve until something is recorded.
  if (recording_span_ != nullptr) {
    recording_span_->reserveTags(size);
  }
}

void NonRecordingSpan::log(SystemTime timestamp, const std::string& event) {
  // A recording span ignores an empty event as well.
  if (recording_span_ == nullptr && event.empty()) {
    return;
  }
  recordingSpan().log(timestamp, event);
}

void NonRecordingSpan::finishSpan() {
  if (recording_span_ != nullptr) {
    recording_span_->finishSpan();
  }
}

void NonRecordingSpan::injectContext(Tracing::TraceContext& trace_context,
                                     const Tracing::UpstreamContext& upstream) {
  if (recording_span_ != nullptr) {
    recording_span_->injectContext(trace_context, upstream);
    return;
  }
  // version "-" trace id "-" span id "-" trace flags, with the sampled flag not set. See
  // https://www.w3.org/TR/trace-context/#traceparent-header.
  std::array<char, 55> traceparent;
  char* out = traceparent.data();
  *out++ = kDefaultVersion[0];
  *out++ = kDefaultVersion[1];
  *out++ = '-';
  writeHex(trace_id_, out);
  out += 2 * trace_id_.size();
  *out++ = '-';
  writeHex(span_id_, out);
  out += 2 * span_id_.size();
  *out++ = '-';
  *out++ = '0';
  *out++ = '0';
  ASSERT(out == traceparent.data() + traceparent.size());
  traceParentHeader().setRefKey(trace_context,
                                absl::string_view(traceparent.data(), traceparent.size()));
  // Also set the tracestate.
  traceStateHeader().setRefKey(trace_context, tracestate_);
}

Tracing::SpanPtr NonRecordingSpan::spawnChild(const Tracing::Config& config,
                                              const std::string& name, SystemTime start_time) {
  if (recording_span_ != nullptr) {
    return recording_span_->spawnChild(config, name, start_time);
  }
  // The same span context a recording span that is not sampled would hand to the tracer.
  SpanContext span_context(kDefaultVersion, toHex(trace_id_), toHex(span_id_), /*sampled=*/false,
                           tracestate_, /*is_remote=*/false);
  return parent_tracer_.startSpan(name, stream_info_, start_time, span_context, {},
                                  ::opentelemetry::proto::trace::v1::Span::SPAN_KIND_CLIENT);
}

void NonRecordingSpan::setSampled(bool sampled) {
  if (recording_span_ == nullptr && !sampled) {
    return;
  }
  recordingSpan().setSampled(sampled);
}

bool NonRecordingSpan::useLocalDecision() const {
  return recording_span_ != nullptr ? recording_span_->useLocalDecision() : use_local_decision_;
}

void NonRecordingSpan::disableLocalDecision() {
  if (recording_span_ != nullptr) {
    recording_span_->disableLocalDecision();
    return;
  }
  use_local_decision_ = false;
}

bool NonRecordingSpan::exportedSpan() const {
  return recording_span_ != nullptr && recording_span_->exportedSpan();
}

std::string NonRecordingSpan::getTraceId() const {
  return recording_span_ != nullptr ? recording_span_->getTraceId() : toHex(trace_id_);
}

std::string NonRecordingSpan::getSpanId() const {
  return recording_span_ != nullptr ? recording_span_->getSpanId() : toHex(span_id_);
}

Tracer::Tracer(OpenTelemetryTraceExporterPtr exporter, Envoy::TimeSource& time_source,
               Random::RandomGenerator& random, Runtime::Loader& runtime,
               Event::Dispatcher& dispatcher, OpenTelemetryTracerStats tracing_stats,
               const ResourceConstSharedPtr resource, SamplerSharedPtr sampler,
               uint64_t max_cache_size, bool set_instrumentation_scope)
    : exporter_(std::move(exporter)), time_source_(time_source), random_(random), runtime_(runtime),
      tracing_stats_(tracing_stats), resource_(resource), sampler_(sampler),
      max_cache_size_(max_cache_size), set_instrumentation_scope_(set_instrumentation_scope),
      non_recording_spans_(Runtime::runtimeFeatureEnabled(
          "envoy.reloadable_features.otel_tracer_non_recording_span")) {
  flush_timer_ = dispatcher.createTimer([this]() -> void {
    tracing_stats_.timer_flushed_.inc();
    flushSpans();
    enableTimer();
  });
  enableTimer();
}

void Tracer::enableTimer() {
  const uint64_t flush_interval =
      runtime_.snapshot().getInteger("tracing.opentelemetry.flush_interval_ms", 5000U);
  flush_timer_->enableTimer(std::chrono::milliseconds(flush_interval));
}

void Tracer::flushSpans() {
  if (span_buffer_.empty()) {
    return;
  }

  if (!exporter_) {
    ENVOY_LOG_EVERY_POW_2(warn,
                          "Skipping log request to OpenTelemetry: no exporter "
                          "configured; dropping {} spans",
                          span_buffer_.size());
    tracing_stats_.spans_dropped_.add(span_buffer_.size());
    span_buffer_.clear();
    return;
  }

  ExportTraceServiceRequest request;
  // A request consists of ResourceSpans.
  ::opentelemetry::proto::trace::v1::ResourceSpans* resource_span = request.add_resource_spans();
  resource_span->set_schema_url(resource_->schema_url_);

  // add resource attributes
  for (auto const& att : resource_->attributes_) {
    opentelemetry::proto::common::v1::KeyValue key_value =
        opentelemetry::proto::common::v1::KeyValue();
    opentelemetry::proto::common::v1::AnyValue value_proto =
        opentelemetry::proto::common::v1::AnyValue();
    value_proto.set_string_value(std::string{att.second});
    key_value.set_key(std::string{att.first});
    *key_value.mutable_value() = value_proto;
    (*resource_span->mutable_resource()->add_attributes()) = key_value;
  }

  ::opentelemetry::proto::trace::v1::ScopeSpans* scope_span = resource_span->add_scope_spans();

  if (set_instrumentation_scope_) {
    // set the instrumentation scope name and version
    *scope_span->mutable_scope()->mutable_name() = "envoy";
    *scope_span->mutable_scope()->mutable_version() = Envoy::VersionInfo::version();
  }

  for (const auto& pending_span : span_buffer_) {
    (*scope_span->add_spans()) = pending_span;
  }
  tracing_stats_.spans_sent_.add(span_buffer_.size());
  if (!exporter_->log(request)) {
    // TODO: should there be any sort of retry or reporting here?
    ENVOY_LOG(trace, "Unsuccessful log request to OpenTelemetry trace collector.");
  }
  span_buffer_.clear();
}

void Tracer::sendSpan(::opentelemetry::proto::trace::v1::Span& span) {
  if (span_buffer_.size() >= max_cache_size_) {
    ENVOY_LOG_EVERY_POW_2(
        warn,
        "Span buffer size exceeded maximum limit. Discarding span. Current size: {}, Max size: {}",
        span_buffer_.size(), max_cache_size_);
    tracing_stats_.spans_dropped_.inc();
    flushSpans();
    return;
  }
  span_buffer_.push_back(span);
  const uint64_t min_flush_spans =
      runtime_.snapshot().getInteger("tracing.opentelemetry.min_flush_spans", 5U);
  if (span_buffer_.size() >= min_flush_spans) {
    flushSpans();
  }
}

Tracing::SpanPtr Tracer::startSpan(const std::string& operation_name,
                                   const StreamInfo::StreamInfo& stream_info, SystemTime start_time,
                                   Tracing::Decision tracing_decision,
                                   OptRef<const Tracing::TraceContext> trace_context,
                                   OTelSpanKind span_kind) {
  // If reached here, then this is main span for request and there is no previous span context.
  // If the custom sampler is set, then the Envoy tracing decision is ignored and the custom sampler
  // should make a sampling decision, otherwise the local Envoy tracing decision is used.
  const bool use_local_decision = sampler_ == nullptr;

  if (non_recording_spans_ && use_local_decision && !tracing_decision.traced) {
    // Not sampled, and no sampler to ask: the span only has to carry the ids. They come from the
    // same three random numbers, in the same order, as the ids of a recording span.
    const uint64_t trace_id_high = random_.random();
    const uint64_t trace_id_low = random_.random();
    const uint64_t span_id = random_.random();
    NonRecordingSpan::TraceId trace_id_bytes;
    storeBigEndian(trace_id_high, trace_id_bytes.data());
    storeBigEndian(trace_id_low, trace_id_bytes.data() + 8);
    NonRecordingSpan::SpanId span_id_bytes;
    storeBigEndian(span_id, span_id_bytes.data());
    return std::make_unique<NonRecordingSpan>(operation_name, stream_info, start_time, time_source_,
                                              *this, span_kind, use_local_decision, trace_id_bytes,
                                              span_id_bytes);
  }

  // Create an Tracers::OpenTelemetry::Span class that will contain the OTel span.
  auto new_span = std::make_unique<Span>(operation_name, stream_info, start_time, time_source_,
                                         *this, span_kind, use_local_decision);
  uint64_t trace_id_high = random_.random();
  uint64_t trace_id = random_.random();
  new_span->setTraceId(absl::StrCat(Hex::uint64ToHex(trace_id_high), Hex::uint64ToHex(trace_id)));
  uint64_t span_id = random_.random();
  new_span->setId(Hex::uint64ToHex(span_id));
  if (sampler_) {
    callSampler(sampler_, stream_info, std::nullopt, *new_span, operation_name, trace_context);
  } else {
    new_span->setSampled(tracing_decision.traced);
  }
  return new_span;
}

Tracing::SpanPtr Tracer::startSpan(const std::string& operation_name,
                                   const StreamInfo::StreamInfo& stream_info, SystemTime start_time,
                                   const SpanContext& parent_context,
                                   OptRef<const Tracing::TraceContext> trace_context,
                                   OTelSpanKind span_kind, bool not_traced_by_envoy) {
  // If reached here, then this is main span for request with a parent context or this is
  // subsequent spans. Ignore the Envoy tracing decision anyway.

  if (non_recording_spans_ && sampler_ == nullptr &&
      (not_traced_by_envoy || !parent_context.sampled())) {
    // Not sampled, and no sampler to ask: the span only has to carry the ids and the tracestate.
    // Ids that are not exactly 32 and 16 hex digits take the recording span below.
    NonRecordingSpan::TraceId trace_id_bytes;
    NonRecordingSpan::SpanId parent_span_id_bytes;
    const bool has_parent_span_id = !parent_context.spanId().empty();
    if (fromHex(parent_context.traceId(), trace_id_bytes) &&
        (!has_parent_span_id || fromHex(parent_context.spanId(), parent_span_id_bytes))) {
      // Generate a new identifier for the span id.
      NonRecordingSpan::SpanId span_id_bytes;
      storeBigEndian(random_.random(), span_id_bytes.data());
      auto new_span = std::make_unique<NonRecordingSpan>(
          operation_name, stream_info, start_time, time_source_, *this, span_kind,
          /*use_local_decision=*/false, trace_id_bytes, span_id_bytes);
      if (has_parent_span_id) {
        new_span->setParent(parent_span_id_bytes, parent_context.isRemote());
      } else {
        new_span->setParentContextIsRemote(parent_context.isRemote());
      }
      if (!parent_context.tracestate().empty()) {
        new_span->setTracestate(parent_context.tracestate());
      }
      return new_span;
    }
  }

  // Create a new span and populate details from the span context.
  auto new_span = std::make_unique<Span>(operation_name, stream_info, start_time, time_source_,
                                         *this, span_kind, false);
  new_span->setParentContextIsRemote(parent_context.isRemote());
  new_span->setTraceId(parent_context.traceId());
  if (!parent_context.spanId().empty()) {
    new_span->setParentId(parent_context.spanId());
  }
  // Generate a new identifier for the span id.
  uint64_t span_id = random_.random();
  new_span->setId(Hex::uint64ToHex(span_id));
  if (sampler_) {
    // Sampler should make a sampling decision and set tracestate
    callSampler(sampler_, stream_info, parent_context, *new_span, operation_name, trace_context);
  } else {
    // Respect the previous span's sampled flag.
    new_span->setSampled(parent_context.sampled());
    if (!parent_context.tracestate().empty()) {
      new_span->setTracestate(parent_context.tracestate());
    }
  }
  return new_span;
}

} // namespace OpenTelemetry
} // namespace Tracers
} // namespace Extensions
} // namespace Envoy
