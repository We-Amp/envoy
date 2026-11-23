#include "source/common/network/raw_buffer_socket.h"

#include "source/common/api/os_sys_calls_impl.h"
#include "source/common/common/assert.h"
#include "source/common/common/empty_string.h"
#include "source/common/http/headers.h"

namespace Envoy {
namespace Network {

namespace {

// Does what IoSocketHandleImpl::read() does for a read without a length limit, and also reports
// whether the read returned fewer bytes than it asked for. IoHandle::read() cannot tell, because
// only the IoHandle knows how large the reservation was.
Api::IoCallUint64Result readAndDetectShortRead(IoHandle& io_handle, Buffer::Instance& buffer,
                                               bool& short_read) {
  Buffer::Reservation reservation = buffer.reserveForRead();
  const uint64_t bytes_requested = reservation.length();
  Api::IoCallUint64Result result =
      io_handle.readv(bytes_requested, reservation.slices(), reservation.numSlices());
  const uint64_t bytes_to_commit = result.ok() ? result.return_value_ : 0;
  ASSERT(bytes_to_commit <= bytes_requested);
  reservation.commit(bytes_to_commit);
  short_read = bytes_to_commit > 0 && bytes_to_commit < bytes_requested;
  return result;
}

} // namespace

void RawBufferSocket::setTransportSocketCallbacks(TransportSocketCallbacks& callbacks) {
  ASSERT(!callbacks_);
  callbacks_ = &callbacks;
}

IoResult RawBufferSocket::doRead(Buffer::Instance& buffer) {
  PostIoAction action = PostIoAction::KeepOpen;
  uint64_t bytes_read = 0;
  bool end_stream = false;
  std::optional<Api::IoError::IoErrorCode> err = std::nullopt;
  // When the connection allows it, a read that returns fewer bytes than it asked for ends the
  // loop: the socket had no more data at that moment, so the read that would only return "no data
  // available" is skipped. See TransportSocketCallbacks::canStopReadingOnShortRead().
  const bool stop_on_short_read = callbacks_->canStopReadingOnShortRead();
  do {
    bool short_read = false;
    Api::IoCallUint64Result result =
        stop_on_short_read ? readAndDetectShortRead(callbacks_->ioHandle(), buffer, short_read)
                           : callbacks_->ioHandle().read(buffer, std::nullopt);

    if (result.ok()) {
      ENVOY_CONN_LOG(trace, "read returns: {}", callbacks_->connection(), result.return_value_);
      if (result.return_value_ == 0) {
        // Remote close.
        end_stream = true;
        break;
      }
      bytes_read += result.return_value_;
      if (callbacks_->shouldDrainReadBuffer()) {
        callbacks_->setTransportSocketIsReadable();
        break;
      }
      if (short_read) {
        break;
      }
    } else {
      // Remote error (might be no data).
      ENVOY_CONN_LOG(trace, "read error: {}, code: {}", callbacks_->connection(),
                     result.err_->getErrorDetails(), static_cast<int>(result.err_->getErrorCode()));
      if (result.err_->getErrorCode() != Api::IoError::IoErrorCode::Again) {
        action = PostIoAction::Close;
        err = result.err_->getErrorCode();
      }
      break;
    }
  } while (true);

  return {action, bytes_read, end_stream, err};
}

IoResult RawBufferSocket::doWrite(Buffer::Instance& buffer, bool end_stream) {
  PostIoAction action;
  uint64_t bytes_written = 0;
  std::optional<Api::IoError::IoErrorCode> err = std::nullopt;
  ASSERT(!shutdown_ || buffer.length() == 0);
  do {
    if (buffer.length() == 0) {
      if (end_stream && !shutdown_) {
        // Ignore the result. This can only fail if the connection failed. In that case, the
        // error will be detected on the next read, and dealt with appropriately.
        callbacks_->ioHandle().shutdown(ENVOY_SHUT_WR);
        shutdown_ = true;
      }
      action = PostIoAction::KeepOpen;
      break;
    }
    Api::IoCallUint64Result result = callbacks_->ioHandle().write(buffer);

    if (result.ok()) {
      ENVOY_CONN_LOG(trace, "write returns: {}", callbacks_->connection(), result.return_value_);
      bytes_written += result.return_value_;
    } else {
      ENVOY_CONN_LOG(trace, "write error: {}, code: {}", callbacks_->connection(),
                     result.err_->getErrorDetails(), static_cast<int>(result.err_->getErrorCode()));
      if (result.err_->getErrorCode() == Api::IoError::IoErrorCode::Again) {
        action = PostIoAction::KeepOpen;
      } else {
        err = result.err_->getErrorCode();
        action = PostIoAction::Close;
      }
      break;
    }
  } while (true);

  return {action, bytes_written, false, err};
}

std::string RawBufferSocket::protocol() const { return EMPTY_STRING; }
absl::string_view RawBufferSocket::failureReason() const { return EMPTY_STRING; }

void RawBufferSocket::onConnected() { callbacks_->raiseEvent(ConnectionEvent::Connected); }

TransportSocketPtr
RawBufferSocketFactory::createTransportSocket(TransportSocketOptionsConstSharedPtr,
                                              Upstream::HostDescriptionConstSharedPtr) const {
  return std::make_unique<RawBufferSocket>();
}

TransportSocketPtr RawBufferSocketFactory::createDownstreamTransportSocket() const {
  return std::make_unique<RawBufferSocket>();
}

bool RawBufferSocketFactory::implementsSecureTransport() const { return false; }

} // namespace Network
} // namespace Envoy
