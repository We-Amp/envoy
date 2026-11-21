Added :ref:`max_bytes_per_datagram <envoy_v3_api_field_config.metrics.v3.StatsdSink.max_bytes_per_datagram>`
to the UDP ``envoy.stat_sinks.statsd`` sink. When it is set, the counters and gauges of a stats flush, and
the histogram samples each thread records during one iteration of its event loop, are sent as
newline-separated metrics in datagrams of at most that size, instead of one metric per datagram. For
all UDP statsd sinks, a ``max_bytes_per_datagram`` above the largest UDP payload of the address family is
now lowered to it, since a larger datagram fails to send.
