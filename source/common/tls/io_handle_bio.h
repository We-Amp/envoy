#pragma once

#include "envoy/network/io_handle.h"

#include "openssl/bio.h"

namespace Envoy {
namespace Extensions {
namespace TransportSockets {
namespace Tls {

/**
 * Creates a custom BIO that can read from/write to an IoHandle. It's equivalent to a socket BIO
 * but instead of relying on access to an fd, it relies on IoHandle APIs for all interactions. The
 * IoHandle must remain valid for the lifetime of the BIO.
 *
 * When staged-write batching is enabled (envoy.reloadable_features.tls_staged_write_batching,
 * default on), writes append ciphertext to a per-BIO pending buffer instead of issuing one
 * writev per record; the owner flushes with BIO_io_handle_flush_pending() (one writev across the
 * whole staged range) and must treat BIO_io_handle_flush_error() as a hard write error. The
 * pending buffer is bounded (256 KiB + one record).
 */
// NOLINTNEXTLINE(readability-identifier-naming)
BIO* BIO_new_io_handle(Envoy::Network::IoHandle* io_handle);

/**
 * Flushes the staged ciphertext to the underlying IoHandle with a single writev. Returns the
 * number of ciphertext bytes still staged (unflushed) after the attempt; a partial flush leaves
 * the remainder staged for a later attempt.
 */
// NOLINTNEXTLINE(readability-identifier-naming)
size_t BIO_io_handle_flush_pending(BIO* bio);

/**
 * Returns the number of ciphertext bytes currently staged in the pending buffer.
 */
// NOLINTNEXTLINE(readability-identifier-naming)
size_t BIO_io_handle_pending_bytes(BIO* bio);

/**
 * Returns true if the last flush attempt hit a hard (non-EAGAIN) write error on the underlying
 * IoHandle. The error is sticky until consumed by this call.
 */
// NOLINTNEXTLINE(readability-identifier-naming)
bool BIO_io_handle_take_flush_error(BIO* bio);

} // namespace Tls
} // namespace TransportSockets
} // namespace Extensions
} // namespace Envoy
