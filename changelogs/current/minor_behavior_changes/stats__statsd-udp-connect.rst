The UDP statsd sinks (``envoy.stat_sinks.statsd``, ``envoy.stat_sinks.dog_statsd`` and
``envoy.stat_sinks.graphite_statsd``) now connect each thread's socket when the address is an IP
address, so a send no longer resolves the route and source address. Unix datagram addresses stay
unconnected. When a send on the connected socket reports a refused earlier datagram (an ICMP port
unreachable), the datagram is sent once more; when the source address or route is gone, the thread
replaces the socket by an unconnected one. This behavior can be reverted by setting the runtime guard
``envoy.reloadable_features.statsd_udp_connect_socket`` to ``false``; the guard is read when the sink
is created.
