A UDP listener that is read with ``recvmmsg`` keeps the receive buffers that a call did not fill
and uses them for the next call, instead of allocating 16 buffers for every call and freeing the
ones that were not filled. After its first read, a listener holds 16 receive buffers of its maximum
datagram size on every worker thread that runs it, also while it is idle, until it is removed. At
the default maximum datagram size of 1,500 bytes that is about 105–110 KiB per listener per worker
thread, depending on the allocator. Buffers are not kept for a maximum datagram size above 16 KiB.
The runtime guard is read when a listener is created, so a change of it applies to the listeners
that are created after the change. This behavior can be reverted by setting the runtime guard
``envoy.reloadable_features.udp_recvmmsg_reuse_buffers`` to ``false``.
