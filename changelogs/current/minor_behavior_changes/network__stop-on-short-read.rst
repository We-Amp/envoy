A connection over a TCP socket without transport security stops reading from the socket when a read
returns fewer bytes than were asked for, instead of reading once more to be told that no data is
available. This saves one system call for every read event that drains the socket, which is the
common case for small messages. To still learn about a peer close or a reset that arrives together
with the last data, the connection also enables the early close file event while it reads and, once
that event is raised, reads until no more data is available as before. Linux only. This behavior can
be reverted by setting the runtime guard
``envoy.reloadable_features.raw_buffer_socket_stop_on_short_read`` to ``false``.
