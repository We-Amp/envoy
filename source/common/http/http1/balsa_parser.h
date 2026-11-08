#pragma once

#include <memory>
#include <string>

#include "source/common/http/http1/parser.h"
#include "source/common/runtime/runtime_features.h"

#include "absl/base/attributes.h"
#include "quiche/balsa/balsa_enums.h"
#include "quiche/balsa/balsa_frame.h"
#include "quiche/balsa/balsa_headers.h"
#include "quiche/balsa/balsa_visitor_interface.h"

namespace Envoy {
namespace Http {
namespace Http1 {

// This class wraps BalsaFrame and BalsaHeaders into a Parser implementation
// to be used by ConnectionImpl.
class BalsaParser : public Parser, public quiche::BalsaVisitorInterface {
public:
  BalsaParser(MessageType type, ParserCallbacks* connection, size_t max_header_length,
              bool enable_trailers, bool allow_custom_methods);
  ~BalsaParser() override = default;

  // Http1::Parser implementation
  size_t execute(const char* slice, size_t len) override;
  void resume() override;
  CallbackResult pause() override;
  ParserStatus getStatus() const override;
  Http::Code statusCode() const override;
  bool isHttp11() const override;
  std::optional<uint64_t> contentLength() const override;
  bool isChunked() const override;
  absl::string_view methodName() const override;
  absl::string_view errorMessage() const override;
  int hasTransferEncoding() const override;
  uint32_t fastPathMessageCount() const override { return fast_path_message_count_; }

private:
  // quiche::BalsaVisitorInterface implementation
  // TODO(bnc): Encapsulate in a private object.
  void OnRawBodyInput(absl::string_view input) override;
  void OnBodyChunkInput(absl::string_view input) override;
  void OnHeaderInput(absl::string_view input) override;
  void OnTrailerInput(absl::string_view input) override;
  void OnTrailers(std::unique_ptr<quiche::BalsaHeaders> trailers) override;
  void ProcessHeaders(const quiche::BalsaHeaders& headers) override;
  void OnRequestFirstLineInput(absl::string_view line_input, absl::string_view method_input,
                               absl::string_view request_uri,
                               absl::string_view version_input) override;
  void OnResponseFirstLineInput(absl::string_view line_input, absl::string_view version_input,
                                absl::string_view status_input,
                                absl::string_view reason_input) override;
  void OnChunkLength(size_t chunk_length) override;
  void OnChunkExtensionInput(absl::string_view input) override;
  void OnInterimHeaders(std::unique_ptr<quiche::BalsaHeaders> headers) override;
  void HeaderDone() override;
  void ContinueHeaderDone() override;
  void MessageDone() override;
  void HandleError(quiche::BalsaFrameEnums::ErrorCode error_code) override;
  void HandleWarning(quiche::BalsaFrameEnums::ErrorCode error_code) override;

  // Shared implementation for ProcessHeaders() and OnTrailers().
  void validateAndProcessHeadersOrTrailersImpl(const quiche::BalsaHeaders& headers, bool trailers);

  // S3 fast path: single-representation header parse. Attempts to parse an entire ordinary
  // message header block directly from `slice` into the header map, bypassing the BalsaHeaders
  // materialization and the per-line callback round trip. Returns the number of bytes consumed
  // (through the block terminator, plus any immediately available Content-Length body) or 0 when
  // the message must go through Balsa instead (see s3-design.md for the trigger list). The scan
  // is all-or-nothing: zero parser state is mutated unless the whole block is validated, so the
  // fallback replays the identical bytes through the unmodified Balsa path.
  size_t tryFastPath(const char* slice, size_t len);
  // Consumes Content-Length body bytes for a fast-path message.
  size_t fastPathBody(const char* slice, size_t len);

  enum class FastBodyState { Inactive, ContentLength };
  FastBodyState fast_body_state_ = FastBodyState::Inactive;
  uint64_t fast_body_remaining_ = 0;
  // Parser-query state for the message parsed by the fast path (Balsa's `headers_` is not
  // populated for such messages).
  bool fast_http11_ = false;
  uint64_t fast_status_code_ = 200;
  // Owned storage for the method view (SSO-sized in practice; views must outlive the committing
  // execute() call, and std::string keeps that safe without a per-request allocation).
  std::string fast_method_storage_;
  absl::string_view fast_method_;
  std::optional<uint64_t> fast_content_length_;
  uint32_t fast_path_message_count_ = 0;


  // Return ParserStatus::Error if `result` is CallbackResult::Error.
  // Return current value of `status_` otherwise.
  // Typical use would be `status_ = convertResult(result);`
  ABSL_MUST_USE_RESULT ParserStatus convertResult(CallbackResult result) const;

  quiche::BalsaFrame framer_;
  quiche::BalsaHeaders headers_;

  const MessageType message_type_ = MessageType::Request;
  ParserCallbacks* connection_ = nullptr;
  const bool enable_trailers_ = false;
  const bool allow_custom_methods_ = false;
  const size_t max_header_length_;
  // Latched on construction, which happens per connection, so that flipping the runtime guard
  // takes effect on new connections without a per-request runtime lookup.
  const bool allow_query_method_ = false;
  bool first_byte_processed_ = false;
  bool headers_done_ = false;
  // True while the current message was parsed by the S3 fast path (Balsa's `headers_` is then
  // not populated for it).
  bool fast_message_ = false;
  // True until the first byte of the second message arrives.
  bool first_message_ = true;
  ParserStatus status_ = ParserStatus::Ok;
  // An error message, often seemingly arbitrary to match http-parser behavior.
  absl::string_view error_message_;
};

} // namespace Http1
} // namespace Http
} // namespace Envoy
