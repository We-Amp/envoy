A connection writes to the socket in the iteration of the event loop in which the data was handed
to it, after the events that were already scheduled, instead of in the next iteration. This saves
one poll for file events for every relayed message when the write is the only work left. It applies
to the first write of an iteration on a connection whose write buffer was empty; other writes are
scheduled as before. Data written by several events of one iteration still goes out in one system
call, but data written in two consecutive iterations is no longer combined. This behavior can be
reverted by setting the runtime guard
``envoy.reloadable_features.connection_write_in_current_iteration`` to ``false``.
