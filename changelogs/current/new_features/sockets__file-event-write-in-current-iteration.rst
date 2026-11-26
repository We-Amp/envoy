Added the runtime guard ``envoy.reloadable_features.file_event_write_in_current_iteration``, which
is ``false`` by default. When it is set to ``true``, an activation of exactly
``FileReadyType::Write`` on an edge-triggered file event, such as the one
``Network::Connection::write()`` makes, runs later in the same event loop iteration instead of in
the next one, which saves a poll with a zero timeout. At most one such activation per file event per
iteration takes this path. Data written before it runs, including data written by other events
reported by the same poll, still goes out in one system call, but data written in two consecutive
iterations is no longer combined, so a connection written in every iteration can make more system
calls and, with TLS, send more records. File events that are not edge-triggered, including the
emulated edge trigger used on Windows, and user space sockets keep the previous behavior. The guard
is read when a file event is created, so a change applies to connections created afterwards. With
the guard enabled, a file event that takes the new path keeps one extra callback for its lifetime.
