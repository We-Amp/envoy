A write to a connection reaches the socket in the event loop iteration in which it was made,
after the events that were already scheduled, instead of in the next iteration. This saves one
poll for file events when the write is the only work left. Data written by several events of one
iteration still goes out together, but data written in two consecutive iterations is no longer
combined into one system call or TLS record. The change is made in ``Event::FileEvent::activate()``
and applies to every activation of exactly ``FileReadyType::Write`` on an edge-triggered file event
that has no injected event pending, at most once per file event per iteration; other activations
are delivered in the next iteration as before. This behavior can be reverted by setting the runtime
guard ``envoy.reloadable_features.connection_write_in_current_iteration`` to ``false``.
