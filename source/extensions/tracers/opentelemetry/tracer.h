#pragma once

#include <cstdint>
#include <optional>

#include "envoy/api/api.h"
#include "envoy/common/optref.h"
#include "envoy/config/trace/v3/opentelemetry.pb.h"
#include "envoy/runtime/runtime.h"
#include "envoy/thread_local/thread_local.h"
#include "envoy/tracing/trace_driver.h"

#include "source/common/common/logger.h"
#include "source/extensions/tracers/common/factory_base.h"
#include "source/extensions/tracers/opentelemetry/grpc_trace_exporter.h"
#include "source/extensions/tracers/opentelemetry/resource_detectors/resource_detector.h"
#include "source/extensions/tracers/opentelemetry/samplers/sampler.h"
#include "source/extensions/tracers/opentelemetry/span_context.h"

#include "absl/container/inlined_vector.h"
#include "absl/strings/escaping.h"

namespace Envoy {
namespace Extensions {
namespace Tracers {
namespace OpenTelemetry {

#define OPENTELEMETRY_TRACER_STATS(COUNTER)                                                        \
  COUNTER(spans_sent)                                                                              \
  COUNTER(timer_flushed)                                                                           \
  COUNTER(spans_dropped)

struct OpenTelemetryTracerStats {
  OPENTELEMETRY_TRACER_STATS(GENERATE_COUNTER_STRUCT)
};

/**
 * OpenTelemetry Tracer. It is stored in TLS and contains the exporter.
 */
class Tracer : Logger::Loggable<Logger::Id::tracing> {
public:
  Tracer(OpenTelemetryTraceExporterPtr exporter, Envoy::TimeSource& time_source,
         Random::RandomGenerator& random, Runtime::Loader& runtime, Event::Dispatcher& dispatcher,
         OpenTelemetryTracerStats tracing_stats, const ResourceConstSharedPtr resource,
         SamplerSharedPtr sampler, uint64_t max_cache_size, bool set_instrumentation_scope = true);

  void sendSpan(::opentelemetry::proto::trace::v1::Span& span);

  Tracing::SpanPtr startSpan(const std::string& operation_name,
                             const StreamInfo::StreamInfo& stream_info, SystemTime start_time,
                             Tracing::Decision tracing_decision,
                             OptRef<const Tracing::TraceContext> trace_context,
                             OTelSpanKind span_kind);

  /**
   * Starts a span that continues the trace of a parent span context.
   * @param tracing_decision the Envoy tracing decision. When it is "not traced" the caller sets
   *        the span to not sampled, whatever the parent context and the sampler say.
   */
  Tracing::SpanPtr startSpan(const std::string& operation_name,
                             const StreamInfo::StreamInfo& stream_info, SystemTime start_time,
                             const SpanContext& previous_span_context,
                             Tracing::Decision tracing_decision,
                             OptRef<const Tracing::TraceContext> trace_context,
                             OTelSpanKind span_kind);

private:
  /**
   * Enables the span-flushing timer.
   */
  void enableTimer();
  /*
   * Removes all spans from the span buffer and sends them to the collector.
   */
  void flushSpans();

  OpenTelemetryTraceExporterPtr exporter_;
  Envoy::TimeSource& time_source_;
  Random::RandomGenerator& random_;
  std::vector<::opentelemetry::proto::trace::v1::Span> span_buffer_;
  Runtime::Loader& runtime_;
  Event::TimerPtr flush_timer_;
  OpenTelemetryTracerStats tracing_stats_;
  const ResourceConstSharedPtr resource_;
  SamplerSharedPtr sampler_;
  uint64_t max_cache_size_;
  const bool set_instrumentation_scope_{true};
};

/**
 * OpenTelemetry tracing implementation of the Envoy Span object.
 * Note that it has a pointer to its parent Tracer to access the shared Exporter.
 */
class Span : Logger::Loggable<Logger::Id::tracing>, public Tracing::Span {
public:
  Span(const std::string& name, const StreamInfo::StreamInfo& stream_info, SystemTime start_time,
       Envoy::TimeSource& time_source, Tracer& parent_tracer, OTelSpanKind span_kind,
       bool use_local_decision = false);

  // Tracing::Span functions
  void setOperation(absl::string_view /*operation*/) override;
  void setTag(absl::string_view /*name*/, absl::string_view /*value*/) override;
  void setTypedTag(absl::string_view name, absl::string_view value,
                   Tracing::TagValueType type) override;
  void reserveTags(size_t size) override;
  void log(SystemTime /*timestamp*/, const std::string& /*event*/) override;
  void finishSpan() override;
  void injectContext(Envoy::Tracing::TraceContext& /*trace_context*/,
                     const Tracing::UpstreamContext&) override;
  Tracing::SpanPtr spawnChild(const Tracing::Config& config, const std::string& name,
                              SystemTime start_time) override;

  /**
   * Set the span's sampled flag.
   */
  void setSampled(bool sampled) override { sampled_ = sampled; };

  /**
   * @return whether the local tracing decision is used by the span.
   */
  bool useLocalDecision() const override { return use_local_decision_; }

  void disableLocalDecision() override { use_local_decision_ = false; }

  /**
   * @return whether the span will be exported to the OTLP backend.
   */
  bool exportedSpan() const override { return sampled_; }

  /**
   * @return whether or not the sampled attribute is set
   */
  bool sampled() const { return sampled_; }

  std::string getBaggage(absl::string_view /*key*/) override { return EMPTY_STRING; };
  void setBaggage(absl::string_view /*key*/, absl::string_view /*value*/) override{};

  // Additional methods

  /**
   * Sets the span's trace id attribute.
   */
  void setTraceId(absl::string_view trace_id_hex) {
    span_.set_trace_id(absl::HexStringToBytes(trace_id_hex));
  }

  std::string getTraceId() const override { return absl::BytesToHexString(span_.trace_id()); };

  std::string getSpanId() const override { return absl::BytesToHexString(span_.span_id()); };

  OTelSpanKind spankind() const { return span_.kind(); }

  /**
   * @return the operation name set on the span
   */
  absl::string_view name() const { return span_.name(); }

  /**
   * Sets the span's id.
   */
  void setId(absl::string_view span_id_hex) {
    span_.set_span_id(absl::HexStringToBytes(span_id_hex));
  }

  std::string spanId() { return absl::BytesToHexString(span_.span_id()); }

  /**
   * Sets the span's parent id.
   */
  void setParentId(absl::string_view parent_span_id_hex) {
    span_.set_parent_span_id(absl::HexStringToBytes(parent_span_id_hex));
  }

  /**
   * Records whether the span's parent context was propagated from a remote parent. Used to
   * populate the is_remote bits of the exported span's flags field.
   */
  void setParentContextIsRemote(bool is_remote) { parent_context_is_remote_ = is_remote; }

  absl::string_view tracestate() const { return span_.trace_state(); }

  /**
   * Sets the span's tracestate.
   */
  void setTracestate(absl::string_view tracestate) {
    span_.set_trace_state(std::string{tracestate});
  }

  /**
   * Sets a span attribute.
   */
  void setAttribute(absl::string_view name, const OTelAttribute& value);

  /**
   * Method to access the span for testing.
   */
  const ::opentelemetry::proto::trace::v1::Span& spanForTest() const { return span_; }

private:
  ::opentelemetry::proto::trace::v1::Span span_;
  const StreamInfo::StreamInfo& stream_info_;
  Tracer& parent_tracer_;
  Envoy::TimeSource& time_source_;
  bool sampled_;
  bool use_local_decision_{false};
  bool parent_context_is_remote_{false};
};

/**
 * The span of a request that is not sampled, for a tracer without a custom sampler. It defers
 * building the OTLP span until the span is set to sampled, which for most such requests never
 * happens.
 *
 * Until then it holds what context propagation needs (the trace id, the span id and the
 * tracestate), what a recording span needs later (name, kind, start time, parent), and a list of
 * the tags and events set on it. When it is set to sampled it creates a recording Span with the
 * same ids and replays the list in order, and from then on forwards every call to that span. So
 * what is propagated and what is exported are the same as with a recording Span from the start.
 *
 * This is not the non-recording span of the OpenTelemetry API, which drops attributes and events
 * and never becomes recording: Envoy allows a span to be set to sampled after it has started (the
 * connection manager does so on a route refresh), and such a span must be exported in full.
 */
class DeferredSpan final : public Tracing::Span {
public:
  /**
   * @param trace_id_high the first 8 bytes of the trace id, as a big-endian number.
   * @param trace_id_low the last 8 bytes of the trace id, as a big-endian number.
   * @param span_id the span id, as a big-endian number.
   */
  DeferredSpan(const std::string& name, const StreamInfo::StreamInfo& stream_info,
               SystemTime start_time, Envoy::TimeSource& time_source, Tracer& parent_tracer,
               OTelSpanKind span_kind, bool use_local_decision, uint64_t trace_id_high,
               uint64_t trace_id_low, uint64_t span_id);

  /**
   * Sets the id of the parent span.
   */
  void setParentId(uint64_t parent_span_id) { parent_span_id_ = parent_span_id; }

  /**
   * Records whether the span's parent context was propagated from a remote parent.
   */
  void setParentContextIsRemote(bool is_remote) { parent_context_is_remote_ = is_remote; }

  /**
   * Sets the tracestate that is propagated with the span.
   */
  void setTracestate(absl::string_view tracestate) { tracestate_ = std::string{tracestate}; }

  // Tracing::Span functions
  void setOperation(absl::string_view operation) override;
  void setTag(absl::string_view name, absl::string_view value) override;
  void setTypedTag(absl::string_view name, absl::string_view value,
                   Tracing::TagValueType type) override;
  void reserveTags(size_t size) override;
  void log(SystemTime timestamp, const std::string& event) override;
  void finishSpan() override;
  void injectContext(Envoy::Tracing::TraceContext& trace_context,
                     const Tracing::UpstreamContext& upstream) override;
  Tracing::SpanPtr spawnChild(const Tracing::Config& config, const std::string& name,
                              SystemTime start_time) override;
  void setSampled(bool sampled) override;
  bool useLocalDecision() const override;
  void disableLocalDecision() override;
  bool exportedSpan() const override;
  std::string getBaggage(absl::string_view /*key*/) override { return EMPTY_STRING; };
  void setBaggage(absl::string_view /*key*/, absl::string_view /*value*/) override{};
  std::string getTraceId() const override;
  std::string getSpanId() const override;

  /**
   * @return the recording span this span has turned into, or nullptr while the span has not been
   *         set to sampled. For testing.
   */
  const OpenTelemetry::Span* recordingSpanForTest() const { return recording_span_.get(); }

private:
  // A tag or an event set on the span before it records.
  struct PendingRecord {
    enum class Kind { Tag, TypedTag, Event };
    Kind kind_;
    std::string name_;
    std::string value_;
    Tracing::TagValueType type_{Tracing::TagValueType::String};
    SystemTime timestamp_;
  };

  /**
   * Creates the recording span and replays the pending tags and events on it.
   */
  void startRecording();

  const StreamInfo::StreamInfo& stream_info_;
  Tracer& parent_tracer_;
  Envoy::TimeSource& time_source_;
  const SystemTime start_time_;
  const OTelSpanKind span_kind_;
  const uint64_t trace_id_high_;
  const uint64_t trace_id_low_;
  const uint64_t span_id_;
  std::optional<uint64_t> parent_span_id_;
  std::string name_;
  std::string tracestate_;
  bool parent_context_is_remote_{false};
  bool use_local_decision_;
  // The connection manager sets two tags on every span it starts.
  absl::InlinedVector<PendingRecord, 2> pending_records_;
  // Set once the span is set to sampled. All calls are forwarded to it from then on.
  std::unique_ptr<OpenTelemetry::Span> recording_span_;
};

using TracerPtr = std::unique_ptr<Tracer>;

} // namespace OpenTelemetry
} // namespace Tracers
} // namespace Extensions
} // namespace Envoy
