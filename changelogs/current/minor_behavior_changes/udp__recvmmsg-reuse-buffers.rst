A UDP listener that is read with ``recvmmsg`` keeps the receive buffers that a call did not fill
and uses them for the next call, instead of allocating 16 buffers for every call and freeing the
ones that were not filled. A listener that has read at least once now holds 16 receive buffers of
its maximum datagram size on every worker thread while it is idle. Buffers are not kept for a
maximum datagram size above 16 KiB. This behavior can be reverted by setting the runtime guard
``envoy.reloadable_features.udp_recvmmsg_reuse_buffers`` to ``false``.
