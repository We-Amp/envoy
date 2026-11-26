Added the runtime guard ``envoy.reloadable_features.connection_write_in_current_iteration``, which
is ``false`` by default. When it is set to ``true``, data written to a connection reaches the socket
in the event loop iteration in which it was written, after the events that were already scheduled,
instead of in the next iteration. This saves a poll for file events with a zero timeout when the
write is the only work left, which lowers CPU use on relayed traffic. Data written by several events
of one iteration still goes out in one system call, but data written in two consecutive iterations is
no longer combined, so a connection that is written in every iteration can make more system calls
and, with TLS, send more records. The change applies to every activation of exactly
``FileReadyType::Write`` on an edge-triggered file event that has no injected event pending, at most
once per file event per iteration. Level-triggered file events, as used on Windows, and user space
sockets keep the previous behavior. The guard is read when a file event is created, so a change of
its value applies to connections created afterwards. With the guard enabled each connection that
writes uses one more callback object, about 200 bytes.
