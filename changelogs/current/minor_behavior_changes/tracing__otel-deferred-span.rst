The OpenTelemetry tracer (``envoy.tracers.opentelemetry``) defers building the OTLP span of a
request that is not sampled, when no custom sampler is configured. Until the span is set to sampled
it only holds the trace id, the span id, the tracestate and the tags and events set on it; the
``traceparent`` and ``tracestate`` headers are propagated as before, with the sampled flag not set.
When the span is set to sampled later, for example on a route refresh, the full span is built with
the same ids, tags and events, so what is exported does not change. Unlike the non-recording span of
the OpenTelemetry API, the deferred span drops nothing. This behavior can be reverted by setting the
runtime guard ``envoy.reloadable_features.otel_tracer_deferred_span`` to ``false``.
