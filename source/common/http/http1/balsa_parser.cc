#include "source/common/http/http1/balsa_parser.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <iterator>

#include "source/common/common/assert.h"
#include "source/common/http/headers.h"
#include "source/common/runtime/runtime_features.h"

#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"

namespace Envoy {
namespace Http {
namespace Http1 {

namespace {

using ::quiche::BalsaFrameEnums;
using ::quiche::BalsaHeaders;

constexpr absl::string_view kColonSlashSlash = "://";
// Response must start with "HTTP".
constexpr char kResponseFirstByte = 'H';
constexpr absl::string_view kHttpVersionPrefix = "HTTP/";
// The end of an ordinary header block: the empty line after the last field.
constexpr absl::string_view kHeaderBlockTerminator = "\r\n\r\n";
constexpr absl::string_view kCRLF = "\r\n";
constexpr absl::string_view kContentLength = "content-length";
constexpr absl::string_view kTransferEncoding = "transfer-encoding";

// RFC 9110 Sections 5.1 and 9.1 define field names and methods as tokens:
// https://www.rfc-editor.org/rfc/rfc9110.html
constexpr char kValidCharacters[] =
    "!#$%&'*+-.0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ^_`abcdefghijklmnopqrstuvwxyz|~";

consteval std::array<uint64_t, 4> makeValidCharacterMask() {
  std::array<uint64_t, 4> mask{};
  for (size_t i = 0; i < sizeof(kValidCharacters) - 1; ++i) {
    const uint8_t index = static_cast<uint8_t>(kValidCharacters[i]);
    mask[index / 64] |= 1ULL << (index % 64);
  }
  return mask;
}

// This keeps the per-character hot path branch-light and avoids a binary search through the valid
// character list for every byte in every HTTP/1 header name.
constexpr std::array<uint64_t, 4> kValidCharacterMask = makeValidCharacterMask();

constexpr bool isValidTokenCharacter(char c) {
  const uint8_t index = static_cast<uint8_t>(c);
  return (kValidCharacterMask[index / 64] & (1ULL << (index % 64))) != 0;
}

static_assert(isValidTokenCharacter('a'));
static_assert(isValidTokenCharacter('Z'));
static_assert(isValidTokenCharacter('-'));
static_assert(!isValidTokenCharacter(':'));
static_assert(!isValidTokenCharacter(' '));

// TODO(#21245): Skip method validation altogether when UHV method validation is
// enabled.
bool isMethodValid(absl::string_view method, bool allow_custom_methods, bool allow_query_method) {
  if (allow_custom_methods) {
    return !method.empty() &&
           std::all_of(method.begin(), method.end(),
                       [](absl::string_view::value_type c) { return isValidTokenCharacter(c); });
  }

  // Must remain sorted: searched by std::binary_search() below.
  static constexpr absl::string_view kValidMethods[] = {
      "ACL",       "BIND",      "CHECKOUT", "CONNECT", "COPY",       "DELETE",     "GET",
      "HEAD",      "LINK",      "LOCK",     "MERGE",   "MKACTIVITY", "MKCALENDAR", "MKCOL",
      "MOVE",      "MSEARCH",   "NOTIFY",   "OPTIONS", "PATCH",      "POST",       "PROPFIND",
      "PROPPATCH", "PURGE",     "PUT",      "QUERY",   "REBIND",     "REPORT",     "SEARCH",
      "SOURCE",    "SUBSCRIBE", "TRACE",    "UNBIND",  "UNLINK",     "UNLOCK",     "UNSUBSCRIBE"};
  static_assert(std::is_sorted(std::begin(kValidMethods), std::end(kValidMethods)));

  const auto* begin = &kValidMethods[0];
  const auto* end = &kValidMethods[std::size(kValidMethods) - 1] + 1;
  if (!std::binary_search(begin, end, method)) {
    return false;
  }

  // Recognizing QUERY (registered by RFC 10008) makes Envoy forward requests it previously
  // rejected with a 400, so the previous behavior remains available by disabling the runtime
  // guard `envoy.reloadable_features.http1_allow_query_method`.
  return allow_query_method || method != Headers::get().MethodValues.Query;
}

// This function is crafted to match the URL validation behavior of the http-parser library.
bool isUrlValid(absl::string_view url, bool is_connect) {
  if (url.empty()) {
    return false;
  }

  // Same set of characters are allowed for path and query.
  const auto is_valid_path_query_char = [](char c) {
    return c == 9 || c == 12 || ('!' <= c && c <= 126);
  };

  // The URL may start with a path.
  if (auto it = url.begin(); *it == '/' || *it == '*') {
    ++it;
    return std::all_of(it, url.end(), is_valid_path_query_char);
  }

  // If method is not CONNECT, parse scheme.
  if (!is_connect) {
    // Scheme must start with alpha and be non-empty.
    auto it = url.begin();
    if (!absl::ascii_isalpha(*it)) {
      return false;
    }
    ++it;
    // Scheme started with an alpha character and the rest of it is alpha, digit, '+', '-' or '.'.
    const auto is_scheme_suffix = [](char c) {
      return absl::ascii_isalpha(c) || absl::ascii_isdigit(c) || c == '+' || c == '-' || c == '.';
    };
    it = std::find_if_not(it, url.end(), is_scheme_suffix);
    url.remove_prefix(it - url.begin());
    if (!absl::StartsWith(url, kColonSlashSlash)) {
      return false;
    }
    url.remove_prefix(kColonSlashSlash.length());
  }

  // Path and query start with the first '/' or '?' character.
  const auto is_path_query_start = [](char c) { return c == '/' || c == '?'; };

  // Divide the rest of the URL into two sections: host, and path/query/fragments.
  auto path_query_begin = std::find_if(url.begin(), url.end(), is_path_query_start);
  const absl::string_view host = url.substr(0, path_query_begin - url.begin());
  const absl::string_view path_query = url.substr(path_query_begin - url.begin());

  const auto valid_host_char = [](char c) {
    return absl::ascii_isalnum(c) || c == '!' || c == '$' || c == '%' || c == '&' || c == '\'' ||
           c == '(' || c == ')' || c == '*' || c == '+' || c == ',' || c == '-' || c == '.' ||
           c == ':' || c == ';' || c == '=' || c == '@' || c == '[' || c == ']' || c == '_' ||
           c == '~';
  };

  // Match http-parser's quirk of allowing any number of '@' characters in host
  // as long as they are not consecutive.
  return std::all_of(host.begin(), host.end(), valid_host_char) && !absl::StrContains(host, "@@") &&
         std::all_of(path_query.begin(), path_query.end(), is_valid_path_query_char);
}

// Returns true if `version_input` is a valid HTTP version string as defined at
// https://www.rfc-editor.org/rfc/rfc9112.html#section-2.3, or empty (for HTTP/0.9).
bool isVersionValid(absl::string_view version_input) {
  if (version_input.empty()) {
    return true;
  }

  if (!absl::StartsWith(version_input, kHttpVersionPrefix)) {
    return false;
  }
  version_input.remove_prefix(kHttpVersionPrefix.size());

  // Version number is in the form of "[0-9].[0-9]".
  return version_input.size() == 3 && absl::ascii_isdigit(version_input[0]) &&
         version_input[1] == '.' && absl::ascii_isdigit(version_input[2]);
}

bool isHeaderNameValid(absl::string_view name) {
  return std::all_of(name.begin(), name.end(),
                     [](absl::string_view::value_type c) { return isValidTokenCharacter(c); });
}

} // anonymous namespace

BalsaParser::BalsaParser(MessageType type, ParserCallbacks* connection, size_t max_header_length,
                         bool enable_trailers, bool allow_custom_methods)
    : message_type_(type), connection_(connection), enable_trailers_(enable_trailers),
      allow_custom_methods_(allow_custom_methods), max_header_length_(max_header_length),
      allow_query_method_(
          Runtime::runtimeFeatureEnabled("envoy.reloadable_features.http1_allow_query_method")) {
  ASSERT(connection_ != nullptr);

  quiche::HttpValidationPolicy http_validation_policy;
  http_validation_policy.disallow_header_continuation_lines = false;
  http_validation_policy.require_header_colon = true;
  http_validation_policy.disallow_multiple_content_length = true;
  http_validation_policy.disallow_transfer_encoding_with_content_length = false;
  http_validation_policy.validate_transfer_encoding = false;
  http_validation_policy.require_content_length_if_body_required = false;
  http_validation_policy.disallow_invalid_header_characters_in_response = true;
  http_validation_policy.disallow_lone_cr_in_chunk_extension = true;

  http_validation_policy.disallow_stray_data_after_chunk =
      Runtime::runtimeFeatureEnabled("envoy.reloadable_features.strict_chunk_parsing");

  framer_.set_http_validation_policy(http_validation_policy);

  framer_.set_balsa_headers(&headers_);
  framer_.set_balsa_visitor(this);
  framer_.set_max_header_length(max_header_length);
  framer_.set_invalid_chars_level(quiche::BalsaFrame::InvalidCharsLevel::kError);
  framer_.EnableTrailers();

  switch (message_type_) {
  case MessageType::Request:
    framer_.set_is_request(true);
    break;
  case MessageType::Response:
    framer_.set_is_request(false);
    break;
  }
}

size_t BalsaParser::execute(const char* slice, size_t len) {
  ASSERT(status_ != ParserStatus::Error);

  if (fast_body_state_ == FastBodyState::ContentLength) {
    return fastPathBody(slice, len);
  }

  if (len > 0 && !first_byte_processed_) {
    fast_message_ = false;
    // S3 fast path: try to consume the whole ordinary header block in one representation.
    // Returns 0 unless the block was fully validated, in which case the parser state has been
    // advanced accordingly and the remaining bytes (if any) belong to the body.
    const size_t fast_consumed = tryFastPath(slice, len);
    if (fast_consumed > 0) {
      return fast_consumed;
    }
  }

  if (len > 0 && !first_byte_processed_) {
    if (first_message_) {
      first_message_ = false;
    } else {
      framer_.Reset();
    }

    if (message_type_ == MessageType::Response && *slice != kResponseFirstByte) {
      status_ = ParserStatus::Error;
      error_message_ = "HPE_INVALID_CONSTANT";
      return 0;
    }

    status_ = convertResult(connection_->onMessageBegin());
    if (status_ == ParserStatus::Error) {
      return 0;
    }

    first_byte_processed_ = true;
  }

  if (len == 0 && headers_done_ && !isChunked() &&
      ((message_type_ == MessageType::Response && hasTransferEncoding()) ||
       !headers_.content_length_valid())) {
    MessageDone();
    return 0;
  }

  if (first_byte_processed_ && len == 0) {
    status_ = ParserStatus::Error;
    error_message_ = "HPE_INVALID_EOF_STATE";
    return 0;
  }

  return framer_.ProcessInput(slice, len);
}

void BalsaParser::resume() {
  ASSERT(status_ != ParserStatus::Error);
  status_ = ParserStatus::Ok;
}

CallbackResult BalsaParser::pause() {
  ASSERT(status_ != ParserStatus::Error);
  status_ = ParserStatus::Paused;
  return CallbackResult::Success;
}

ParserStatus BalsaParser::getStatus() const { return status_; }

Http::Code BalsaParser::statusCode() const {
  if (fast_message_) {
    return static_cast<Http::Code>(fast_status_code_);
  }
  return static_cast<Http::Code>(headers_.parsed_response_code());
}

bool BalsaParser::isHttp11() const {
  if (fast_message_) {
    return fast_http11_;
  }
  if (message_type_ == MessageType::Request) {
    return absl::EndsWith(headers_.first_line(), Http::Headers::get().ProtocolStrings.Http11String);
  } else {
    return absl::StartsWith(headers_.first_line(),
                            Http::Headers::get().ProtocolStrings.Http11String);
  }
}

std::optional<uint64_t> BalsaParser::contentLength() const {
  if (fast_message_) {
    return fast_content_length_;
  }
  if (!headers_.content_length_valid()) {
    return std::nullopt;
  }
  return headers_.content_length();
}

bool BalsaParser::isChunked() const {
  if (fast_message_) {
    // The fast path never handles chunked messages; they fall back to Balsa.
    return false;
  }
  return headers_.transfer_encoding_is_chunked();
}

absl::string_view BalsaParser::methodName() const {
  if (fast_message_) {
    return fast_method_;
  }
  return headers_.request_method();
}

absl::string_view BalsaParser::errorMessage() const { return error_message_; }

int BalsaParser::hasTransferEncoding() const {
  if (fast_message_) {
    // The fast path never handles Transfer-Encoding; such messages fall back to Balsa.
    return 0;
  }
  return headers_.HasHeader(Http::Headers::get().TransferEncoding);
}

void BalsaParser::OnRawBodyInput(absl::string_view /*input*/) {}

void BalsaParser::OnBodyChunkInput(absl::string_view input) {
  if (status_ == ParserStatus::Error) {
    return;
  }

  connection_->bufferBody(input.data(), input.size());
}

void BalsaParser::OnHeaderInput(absl::string_view /*input*/) {}
void BalsaParser::OnTrailerInput(absl::string_view /*input*/) {}

void BalsaParser::ProcessHeaders(const BalsaHeaders& headers) {
  validateAndProcessHeadersOrTrailersImpl(headers, /* trailers = */ false);
}
void BalsaParser::OnTrailers(std::unique_ptr<quiche::BalsaHeaders> trailers) {
  validateAndProcessHeadersOrTrailersImpl(*trailers, /* trailers = */ true);
}

void BalsaParser::OnRequestFirstLineInput(absl::string_view /*line_input*/,
                                          absl::string_view method_input,
                                          absl::string_view request_uri,
                                          absl::string_view version_input) {
  if (status_ == ParserStatus::Error) {
    return;
  }
  if (!isMethodValid(method_input, allow_custom_methods_, allow_query_method_)) {
    status_ = ParserStatus::Error;
    error_message_ = "HPE_INVALID_METHOD";
    return;
  }
  const bool is_connect = method_input == Headers::get().MethodValues.Connect;
  if (!isUrlValid(request_uri, is_connect)) {
    status_ = ParserStatus::Error;
    error_message_ = "HPE_INVALID_URL";
    return;
  }
  if (!isVersionValid(version_input)) {
    status_ = ParserStatus::Error;
    error_message_ = "HPE_INVALID_VERSION";
    return;
  }
  status_ = convertResult(connection_->onUrl(request_uri.data(), request_uri.size()));
}

void BalsaParser::OnResponseFirstLineInput(absl::string_view /*line_input*/,
                                           absl::string_view version_input,
                                           absl::string_view /*status_input*/,
                                           absl::string_view reason_input) {
  if (status_ == ParserStatus::Error) {
    return;
  }
  if (!isVersionValid(version_input)) {
    status_ = ParserStatus::Error;
    error_message_ = "HPE_INVALID_VERSION";
    return;
  }
  status_ = convertResult(connection_->onStatus(reason_input.data(), reason_input.size()));
}

void BalsaParser::OnChunkLength(size_t chunk_length) {
  if (status_ == ParserStatus::Error) {
    return;
  }
  const bool is_final_chunk = chunk_length == 0;
  connection_->onChunkHeader(is_final_chunk);
}

void BalsaParser::OnChunkExtensionInput(absl::string_view /*input*/) {}

void BalsaParser::OnInterimHeaders(std::unique_ptr<BalsaHeaders> /*headers*/) {}

void BalsaParser::HeaderDone() {
  if (status_ == ParserStatus::Error) {
    return;
  }
  headers_done_ = true;
  CallbackResult result = connection_->onHeadersComplete();
  status_ = convertResult(result);
  if (result == CallbackResult::NoBody || result == CallbackResult::NoBodyData) {
    MessageDone();
  }
}

void BalsaParser::ContinueHeaderDone() {}

void BalsaParser::MessageDone() {
  if (status_ == ParserStatus::Error ||
      // In the case of early 1xx, MessageDone() can be called twice in a row.
      // The !first_byte_processed_ check is to make this function idempotent.
      !first_byte_processed_) {
    return;
  }
  status_ = convertResult(connection_->onMessageComplete());
  first_byte_processed_ = false;
  headers_done_ = false;
}

void BalsaParser::HandleError(BalsaFrameEnums::ErrorCode error_code) {
  status_ = ParserStatus::Error;
  switch (error_code) {
  case BalsaFrameEnums::UNKNOWN_TRANSFER_ENCODING:
    error_message_ = "unsupported transfer encoding";
    break;
  case BalsaFrameEnums::INVALID_CHUNK_LENGTH:
    error_message_ = "HPE_INVALID_CHUNK_SIZE";
    break;
  case BalsaFrameEnums::HEADERS_TOO_LONG:
    error_message_ = "headers size exceeds limit";
    break;
  case BalsaFrameEnums::TRAILER_TOO_LONG:
    error_message_ = "trailers size exceeds limit";
    break;
  case BalsaFrameEnums::TRAILER_MISSING_COLON:
    error_message_ = "HPE_INVALID_HEADER_TOKEN";
    break;
  case BalsaFrameEnums::INVALID_HEADER_CHARACTER:
    error_message_ = "header value contains invalid chars";
    break;
  case BalsaFrameEnums::MULTIPLE_CONTENT_LENGTH_KEYS:
    error_message_ = "HPE_UNEXPECTED_CONTENT_LENGTH";
    break;
  default:
    error_message_ = BalsaFrameEnums::ErrorCodeToString(error_code);
  }
}

void BalsaParser::HandleWarning(BalsaFrameEnums::ErrorCode error_code) {
  if (error_code == BalsaFrameEnums::TRAILER_MISSING_COLON) {
    HandleError(error_code);
  }
}

size_t BalsaParser::tryFastPath(const char* slice, size_t len) {
  const absl::string_view input(slice, len);

  if (message_type_ == MessageType::Response && input.front() != kResponseFirstByte) {
    return 0;
  }

  // The entire header block must be present in this slice, within the header-size limit.
  // Anything else (partial blocks, oversized blocks) goes through Balsa unchanged, which
  // accumulates across slices and reports HEADERS_TOO_LONG identically.
  const size_t window_len = std::min(len, max_header_length_);
  const size_t term = input.substr(0, window_len).find(kHeaderBlockTerminator);
  if (term == absl::string_view::npos) {
    return 0;
  }
  const size_t block_len = term + kHeaderBlockTerminator.size();
  const absl::string_view block = input.substr(0, block_len);

  // ---- Scan phase: validate the whole block. Zero parser state is mutated here, so any
  // deviation below simply returns 0 and the Balsa path replays the identical bytes.
  const size_t first_line_end = block.find(kCRLF);
  ASSERT(first_line_end != absl::string_view::npos);
  const absl::string_view first_line = block.substr(0, first_line_end);

  absl::string_view method;
  absl::string_view url;
  absl::string_view version;
  absl::string_view reason;
  uint64_t status_code = 0;
  if (message_type_ == MessageType::Request) {
    const size_t first_sp = first_line.find(' ');
    if (first_sp == absl::string_view::npos) {
      return 0;
    }
    method = first_line.substr(0, first_sp);
    const size_t second_sp = first_line.find(' ', first_sp + 1);
    if (second_sp == absl::string_view::npos) {
      return 0;
    }
    url = first_line.substr(first_sp + 1, second_sp - first_sp - 1);
    version = first_line.substr(second_sp + 1);
    if (method.empty() || url.empty() || version.empty() || !isVersionValid(version) ||
        !isMethodValid(method, allow_custom_methods_, allow_query_method_) ||
        !isUrlValid(url, /* is_connect = */ false) ||
        method == Headers::get().MethodValues.Connect) {
      return 0;
    }
  } else {
    const size_t first_sp = first_line.find(' ');
    if (first_sp == absl::string_view::npos || !isVersionValid(first_line.substr(0, first_sp)) ||
        first_line.size() < first_sp + 4) {
      return 0;
    }
    version = first_line.substr(0, first_sp);
    const absl::string_view code = first_line.substr(first_sp + 1, 3);
    if (!absl::ascii_isdigit(code[0]) || !absl::ascii_isdigit(code[1]) ||
        !absl::ascii_isdigit(code[2])) {
      return 0;
    }
    status_code = (code[0] - '0') * 100 + (code[1] - '0') * 10 + (code[2] - '0');
    if (first_line.size() == first_sp + 4) {
      // No reason phrase: leave this shape to Balsa for exact parity.
      return 0;
    }
    if (first_line[first_sp + 4] != ' ') {
      return 0;
    }
    reason = first_line.substr(first_sp + 5);
  }

  const bool is_http11 = version == Http::Headers::get().ProtocolStrings.Http11String;

  // Field lines: strict CRLF framing only. Line-by-line pass collecting framing.
  bool te_present = false;
  bool cl_seen = false;
  uint64_t content_length = 0;
  size_t pos = first_line_end + kCRLF.size();
  while (pos + kCRLF.size() <= block.size()) {
    size_t eol = block.find(kCRLF, pos);
    ASSERT(eol != absl::string_view::npos && eol <= block.size() - kCRLF.size());
    const absl::string_view line = block.substr(pos, eol - pos);
    pos = eol + kCRLF.size();
    if (line.empty()) {
      // End of the header block.
      break;
    }
    const char front = line.front();
    if (front == ' ' || front == '\t') {
      // obs-fold continuation: Balsa joins these; leave the message to it.
      return 0;
    }
    if (line.find('\r') != absl::string_view::npos || line.find('\n') != absl::string_view::npos) {
      // Bare CR or LF inside a line: not strict CRLF framing.
      return 0;
    }
    const size_t colon = line.find(':');
    if (colon == absl::string_view::npos || colon == 0) {
      return 0;
    }
    const absl::string_view name = line.substr(0, colon);
    if (name.back() == ' ' || name.back() == '\t' || !isHeaderNameValid(name)) {
      return 0;
    }
    // Framing sniffing on the lowercased name. Cheap compare: names here are short, and any
    // name longer than the buffer cannot be Content-Length or Transfer-Encoding.
    char lower_buf[32];
    if (name.size() <= sizeof(lower_buf)) {
      for (size_t i = 0; i < name.size(); ++i) {
        lower_buf[i] = absl::ascii_tolower(name[i]);
      }
      const absl::string_view lower_name(lower_buf, name.size());
      if (lower_name == kTransferEncoding) {
        te_present = true;
      } else if (lower_name == kContentLength) {
        if (cl_seen) {
          // Multiple Content-Length fields: quiche's disallow_multiple_content_length policy
          // errors; keep exact parity by falling back.
          return 0;
        }
        cl_seen = true;
        absl::string_view value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
          value.remove_prefix(1);
        }
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
          value.remove_suffix(1);
        }
        if (value.empty() || !absl::SimpleAtoi(value, &content_length)) {
          return 0;
        }
      }
    }
  }
  if (te_present) {
    // Chunked or otherwise transfer-encoded messages keep the Balsa path.
    return 0;
  }

  // Framing decision, matching Balsa's semantics for ordinary messages.
  bool has_body = cl_seen && content_length > 0;
  if (message_type_ == MessageType::Response) {
    const bool no_body_status = status_code < 200 || status_code == 204 || status_code == 304;
    if (no_body_status) {
      // Balsa ends these messages at the headers regardless of Content-Length.
      has_body = false;
    } else if (!cl_seen) {
      // Body until connection close: Balsa handles the EOF framing.
      return 0;
    }
  }

  // ---- Commit phase: the block is fully validated ordinary input. Emit the exact callback
  // sequence the Balsa path would have produced.
  first_message_ = false;
  status_ = convertResult(connection_->onMessageBegin());
  if (status_ == ParserStatus::Error) {
    return 0;
  }
  if (message_type_ == MessageType::Request) {
    status_ = convertResult(connection_->onUrl(url.data(), url.size()));
  } else {
    status_ = convertResult(connection_->onStatus(reason.data(), reason.size()));
  }
  if (status_ == ParserStatus::Error) {
    return 0;
  }

  fast_message_ = true;
  fast_http11_ = is_http11;
  fast_status_code_ = status_code;
  fast_method_storage_ = std::string(method);
  fast_method_ = fast_method_storage_;
  fast_content_length_ = cl_seen ? std::optional<uint64_t>(content_length) : std::nullopt;
  first_byte_processed_ = true;
  headers_done_ = true;
  ++fast_path_message_count_;

  // Second pass over the field lines: one direct-insertion call per field, replacing the
  // per-line onHeaderField/onHeaderValue accumulation round trip.
  pos = first_line_end + kCRLF.size();
  while (pos + kCRLF.size() <= block.size()) {
    const size_t eol = block.find(kCRLF, pos);
    const absl::string_view line = block.substr(pos, eol - pos);
    pos = eol + kCRLF.size();
    if (line.empty()) {
      break;
    }
    const size_t colon = line.find(':');
    status_ = convertResult(connection_->onFastHeaderField(line.substr(0, colon),
                                                           line.substr(colon + 1)));
    if (status_ == ParserStatus::Error) {
      return 0;
    }
  }

  status_ = convertResult(connection_->onHeadersComplete());
  if (status_ == ParserStatus::Error) {
    return 0;
  }

  const size_t header_bytes = block_len;
  if (!has_body) {
    // Match Balsa: a message with no body completes right after the headers.
    MessageDone();
    return header_bytes;
  }
  if (status_ == ParserStatus::Paused) {
    // The codec paused during the header callbacks (e.g. the connection began closing while
    // decodeHeaders ran): stop at the headers and leave the body unconsumed, exactly like the
    // baseline parser, which feeds no body bytes after a pause in onHeadersCompleteBase.
    return header_bytes;
  }

  fast_body_state_ = FastBodyState::ContentLength;
  fast_body_remaining_ = content_length;
  return header_bytes + fastPathBody(slice + header_bytes, len - header_bytes);
}

size_t BalsaParser::fastPathBody(const char* slice, size_t len) {
  ASSERT(fast_body_state_ == FastBodyState::ContentLength);
  if (len == 0) {
    // EOF/flush while the body is incomplete.
    if (fast_body_remaining_ > 0) {
      status_ = ParserStatus::Error;
      error_message_ = "HPE_INVALID_EOF_STATE";
    }
    return 0;
  }
  const size_t n = std::min<size_t>(len, fast_body_remaining_);
  connection_->bufferBody(slice, n);
  fast_body_remaining_ -= n;
  if (fast_body_remaining_ == 0) {
    fast_body_state_ = FastBodyState::Inactive;
    MessageDone();
  }
  return n;
}

void BalsaParser::validateAndProcessHeadersOrTrailersImpl(const quiche::BalsaHeaders& headers,
                                                          bool trailers) {
  for (const auto& [key, value] : headers.lines()) {
    if (status_ == ParserStatus::Error) {
      return;
    }

    if (!isHeaderNameValid(key)) {
      status_ = ParserStatus::Error;
      error_message_ = "HPE_INVALID_HEADER_TOKEN";
      return;
    }

    if (trailers && !enable_trailers_) {
      continue;
    }

    status_ = convertResult(connection_->onHeaderField(key.data(), key.length()));
    if (status_ == ParserStatus::Error) {
      return;
    }

    // Remove CR and LF characters to match http-parser behavior.
    auto is_cr_or_lf = [](char c) { return c == '\r' || c == '\n'; };
    if (std::any_of(value.begin(), value.end(), is_cr_or_lf)) {
      std::string value_without_cr_or_lf;
      value_without_cr_or_lf.reserve(value.size());
      for (char c : value) {
        if (!is_cr_or_lf(c)) {
          value_without_cr_or_lf.push_back(c);
        }
      }
      status_ = convertResult(connection_->onHeaderValue(value_without_cr_or_lf.data(),
                                                         value_without_cr_or_lf.length()));
    } else {
      // No need to copy if header value does not contain CR or LF.
      status_ = convertResult(connection_->onHeaderValue(value.data(), value.length()));
    }
  }
}

ParserStatus BalsaParser::convertResult(CallbackResult result) const {
  return result == CallbackResult::Error ? ParserStatus::Error : status_;
}

} // namespace Http1
} // namespace Http
} // namespace Envoy
