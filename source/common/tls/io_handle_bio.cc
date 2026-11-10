#include "source/common/tls/io_handle_bio.h"

#include <vector>

#include "envoy/buffer/buffer.h"
#include "envoy/common/platform.h"
#include "envoy/network/io_handle.h"

#include "source/common/runtime/runtime_features.h"

#include "openssl/bio.h"
#include "openssl/err.h"

namespace Envoy {
namespace Extensions {
namespace TransportSockets {
namespace Tls {

namespace {

// Bound for the staged ciphertext buffer. When a write would exceed it, a best-effort flush is
// attempted first; the hard bound is therefore kMaxPendingBytes plus one record.
constexpr size_t kMaxPendingBytes = 256 * 1024;

// Per-BIO write-batching state. The IoHandle must remain valid for the lifetime of the BIO.
struct BioContext {
  Envoy::Network::IoHandle* io_handle;
  // Staged ciphertext awaiting a flush. flushed is the byte prefix of pending already handed to
  // the socket; compaction happens on the next flush attempt.
  std::vector<uint8_t> pending;
  size_t flushed{0};
  // Set when a flush attempt hit a hard (non-EAGAIN) write error.
  bool flush_error{false};
};

// NOLINTNEXTLINE(readability-identifier-naming)
inline BioContext* bio_context(BIO* bio) {
  // Null for foreign BIOs (e.g. BIO pairs): the SSL layer can hold a wbio that is not ours.
  if (bio == nullptr || BIO_method_type(bio) != BIO_TYPE_SOCKET) {
    return nullptr;
  }
  return reinterpret_cast<BioContext*>(BIO_get_data(bio));
}

// NOLINTNEXTLINE(readability-identifier-naming)
inline Envoy::Network::IoHandle* bio_io_handle(BIO* bio) { return bio_context(bio)->io_handle; }

// Best-effort flush of the staged ciphertext: one writev over the whole unflushed range. On
// EAGAIN/partial writes the remainder stays staged; on a hard error the sticky flush_error flag
// is set and the data stays staged (the owner closes the connection).
void flush_pending(BioContext* ctx) {
  size_t unflushed = ctx->pending.size() - ctx->flushed;
  if (unflushed == 0) {
    return;
  }
  if (ctx->flushed > 0) {
    // Compact so a single slice covers everything outstanding.
    memmove(ctx->pending.data(), ctx->pending.data() + ctx->flushed, unflushed);
    ctx->pending.resize(unflushed);
    ctx->flushed = 0;
  }
  Envoy::Buffer::RawSlice slice;
  slice.mem_ = ctx->pending.data();
  slice.len_ = unflushed;
  auto result = ctx->io_handle->writev(&slice, 1);
  if (!result.ok()) {
    const auto err = result.err_->getErrorCode();
    if (err != Api::IoError::IoErrorCode::Again &&
        err != Api::IoError::IoErrorCode::Interrupt) {
      ERR_put_error(ERR_LIB_SYS, 0, result.err_->getSystemErrorCode(), __FILE__, __LINE__);
      ctx->flush_error = true;
    }
    return;
  }
  ctx->flushed = result.return_value_;
}

// NOLINTNEXTLINE(readability-identifier-naming)
int io_handle_read(BIO* b, char* out, int outl) {
  if (out == nullptr) {
    return 0;
  }

  Envoy::Buffer::RawSlice slice;
  slice.mem_ = out;
  slice.len_ = outl;
  auto* io_handle = bio_io_handle(b);
  auto result = io_handle->readv(outl, &slice, 1);
  BIO_clear_retry_flags(b);
  if (!result.ok()) {
    auto err = result.err_->getErrorCode();
    if (err == Api::IoError::IoErrorCode::Again || err == Api::IoError::IoErrorCode::Interrupt) {
      BIO_set_retry_read(b);
    } else {
      ERR_put_error(ERR_LIB_SYS, 0, result.err_->getSystemErrorCode(), __FILE__, __LINE__);
    }
    return -1;
  }
  // On EOF (readv returned 0 successfully), check SO_ERROR for a pending TCP RST.
  // Some peers tear the connection down with RST instead of FIN; readv returns 0
  // in that case but a connection error is pending on the socket. Push it onto
  // the error queue so the SSL layer can detect it via drainErrorQueue().
  if (result.return_value_ == 0 &&
      Runtime::runtimeFeatureEnabled(
          "envoy.reloadable_features.ssl_socket_report_connection_reset")) {
    int so_error = 0;
    socklen_t so_error_len = sizeof(so_error);
    auto opt_result = io_handle->getOption(SOL_SOCKET, SO_ERROR, &so_error, &so_error_len);
    if (opt_result.return_value_ == 0 && so_error != 0) {
      ERR_put_error(ERR_LIB_SYS, 0, so_error, __FILE__, __LINE__);
    }
  }
  return result.return_value_;
}

// NOLINTNEXTLINE(readability-identifier-naming)
int io_handle_write(BIO* b, const char* in, int inl) {
  BIO_clear_retry_flags(b);
  auto* ctx = bio_context(b);
  if (ctx == nullptr) {
    return 0;
  }
  if (Runtime::runtimeFeatureEnabled("envoy.reloadable_features.tls_staged_write_batching")) {
    // Stage the ciphertext; the owner flushes with a single writev across all staged records.
    // The staging buffer is bounded: above the cap a best-effort flush runs first, so the hard
    // bound is kMaxPendingBytes plus one record. No syscalls happen here, so SSL_write never
    // reports WANT_WRITE from this BIO.
    if (ctx->pending.size() - ctx->flushed >= kMaxPendingBytes) {
      flush_pending(ctx);
    }
    ctx->pending.insert(ctx->pending.end(), in, in + inl);
    return inl;
  }
  Envoy::Buffer::RawSlice slice;
  slice.mem_ = const_cast<char*>(in);
  slice.len_ = inl;
  auto result = ctx->io_handle->writev(&slice, 1);
  if (!result.ok()) {
    auto err = result.err_->getErrorCode();
    if (err == Api::IoError::IoErrorCode::Again || err == Api::IoError::IoErrorCode::Interrupt) {
      BIO_set_retry_write(b);
    } else {
      ERR_put_error(ERR_LIB_SYS, 0, result.err_->getSystemErrorCode(), __FILE__, __LINE__);
    }
    return -1;
  }
  return result.return_value_;
}

// NOLINTNEXTLINE(readability-identifier-naming)
long io_handle_ctrl(BIO*, int cmd, long, void*) {
  long ret = 1;

  switch (cmd) {
  case BIO_CTRL_FLUSH:
    ret = 1;
    break;
  default:
    ret = 0;
    break;
  }
  return ret;
}

// NOLINTNEXTLINE(readability-identifier-naming)
int io_handle_destroy(BIO* bio) {
  delete bio_context(bio);
  BIO_set_data(bio, nullptr);
  return 1;
}

// NOLINTNEXTLINE(readability-identifier-naming)
const BIO_METHOD* BIO_s_io_handle(void) {
  static const BIO_METHOD* method = [&] {
    BIO_METHOD* ret = BIO_meth_new(BIO_TYPE_SOCKET, "io_handle");
    RELEASE_ASSERT(ret != nullptr, "");
    RELEASE_ASSERT(BIO_meth_set_read(ret, io_handle_read), "");
    RELEASE_ASSERT(BIO_meth_set_write(ret, io_handle_write), "");
    RELEASE_ASSERT(BIO_meth_set_ctrl(ret, io_handle_ctrl), "");
    RELEASE_ASSERT(BIO_meth_set_destroy(ret, io_handle_destroy), "");
    return ret;
  }();
  return method;
}

} // namespace

// NOLINTNEXTLINE(readability-identifier-naming)
size_t BIO_io_handle_flush_pending(BIO* bio) {
  auto* ctx = bio_context(bio);
  if (ctx == nullptr) {
    return 0;
  }
  flush_pending(ctx);
  return ctx->pending.size() - ctx->flushed;
}

// NOLINTNEXTLINE(readability-identifier-naming)
size_t BIO_io_handle_pending_bytes(BIO* bio) {
  auto* ctx = bio_context(bio);
  if (ctx == nullptr) {
    return 0;
  }
  return ctx->pending.size() - ctx->flushed;
}

// NOLINTNEXTLINE(readability-identifier-naming)
bool BIO_io_handle_take_flush_error(BIO* bio) {
  auto* ctx = bio_context(bio);
  if (ctx == nullptr) {
    return false;
  }
  const bool error = ctx->flush_error;
  ctx->flush_error = false;
  return error;
}

// NOLINTNEXTLINE(readability-identifier-naming)
BIO* BIO_new_io_handle(Envoy::Network::IoHandle* io_handle) {
  BIO* b;

  b = BIO_new(BIO_s_io_handle());
  RELEASE_ASSERT(b != nullptr, "");

  // Initialize the BIO with the per-socket write-batching context.
  BIO_set_data(b, new BioContext{io_handle, {}, 0, false});
  BIO_set_init(b, 1);

  return b;
}

} // namespace Tls
} // namespace TransportSockets
} // namespace Extensions
} // namespace Envoy
