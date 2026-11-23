The OpenTelemetry tracer (``envoy.tracers.opentelemetry``) no longer builds an OTLP span for a
request that is not sampled, when no custom sampler is configured. Such a request gets a span that
only carries the trace id, the span id and the tracestate; the ``traceparent`` and ``tracestate``
headers are propagated as before, with the sampled flag not set. The span turns into a recording
span, with the same ids, the first time something is recorded on it or it is set to sampled, so
what is exported does not change. This behavior can be reverted by setting the runtime guard
``envoy.reloadable_features.otel_tracer_non_recording_span`` to ``false``; the guard is read when
the tracer is created.
