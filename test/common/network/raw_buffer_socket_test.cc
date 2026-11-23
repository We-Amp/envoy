#include "source/common/buffer/buffer_impl.h"
#include "source/common/network/io_socket_error_impl.h"
#include "source/common/network/raw_buffer_socket.h"
#include "source/common/network/transport_socket_options_impl.h"

#include "test/mocks/network/io_handle.h"
#include "test/mocks/network/mocks.h"
#include "test/test_common/network_utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;
using testing::InSequence;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;
using testing::ReturnRef;

namespace Envoy {
namespace Network {

TEST(RawBufferSocketFactory, RawBufferSocketFactory) {
  UpstreamTransportSocketFactoryPtr factory = Envoy::Network::Test::createRawBufferSocketFactory();
  EXPECT_FALSE(factory->implementsSecureTransport());
  std::vector<uint8_t> keys;
  factory->hashKey(keys, nullptr);
  EXPECT_EQ(keys.size(), 0);
  auto options = std::make_shared<TransportSocketOptionsImpl>("server");
  factory->hashKey(keys, options);
  EXPECT_GT(keys.size(), 0);
}

// The read loop of the raw buffer socket. The parameter is the answer of
// TransportSocketCallbacks::canStopReadingOnShortRead(): with false the socket reads through
// IoHandle::read() until no more data is available, with true it reads through IoHandle::readv()
// and stops at the first read that returns fewer bytes than it asked for.
class RawBufferSocketReadTest : public testing::TestWithParam<bool> {
public:
  RawBufferSocketReadTest() {
    ON_CALL(callbacks_, ioHandle()).WillByDefault(ReturnRef(io_handle_));
    ON_CALL(callbacks_, canStopReadingOnShortRead()).WillByDefault(Return(stopOnShortRead()));
    socket_.setTransportSocketCallbacks(callbacks_);
  }

  bool stopOnShortRead() const { return GetParam(); }

  // Expects one read that returns `bytes` bytes of data, or that fills all the space it was given
  // if `bytes` is kFull.
  void expectDataRead(uint64_t bytes) {
    if (stopOnShortRead()) {
      EXPECT_CALL(io_handle_, readv(_, _, _))
          .WillOnce(Invoke([bytes](uint64_t max_length, Buffer::RawSlice* slices,
                                   uint64_t num_slice) -> Api::IoCallUint64Result {
            uint64_t available = 0;
            for (uint64_t i = 0; i < num_slice; i++) {
              memset(slices[i].mem_, 'a', slices[i].len_);
              available += slices[i].len_;
            }
            // The socket asks for exactly what it reserved.
            EXPECT_EQ(available, max_length);
            const uint64_t to_read = bytes == kFull ? max_length : bytes;
            EXPECT_LE(to_read, max_length);
            return {to_read, Api::IoError::none()};
          }));
    } else {
      EXPECT_CALL(io_handle_, read(_, _))
          .WillOnce(Invoke([bytes](Buffer::Instance& buffer,
                                   std::optional<uint64_t> max_length) -> Api::IoCallUint64Result {
            EXPECT_FALSE(max_length.has_value());
            const uint64_t to_read = bytes == kFull ? kFullReadSize : bytes;
            buffer.add(std::string(to_read, 'a'));
            return {to_read, Api::IoError::none()};
          }));
    }
  }

  // Expects one read that returns `result`.
  void expectRead(std::function<Api::IoCallUint64Result()> result) {
    if (stopOnShortRead()) {
      EXPECT_CALL(io_handle_, readv(_, _, _))
          .WillOnce(Invoke([result](uint64_t, Buffer::RawSlice*, uint64_t) { return result(); }));
    } else {
      EXPECT_CALL(io_handle_, read(_, _))
          .WillOnce(
              Invoke([result](Buffer::Instance&, std::optional<uint64_t>) { return result(); }));
    }
  }

  static Api::IoCallUint64Result again() { return {0, IoSocketError::getIoSocketEagainError()}; }
  static Api::IoCallUint64Result endOfStream() { return {0, Api::IoError::none()}; }
  static Api::IoCallUint64Result connectionReset() {
    return {0, IoSocketError::create(SOCKET_ERROR_CONNRESET)};
  }

  static constexpr uint64_t kFull = UINT64_MAX;
  // What a read that fills its space returns in the IoHandle::read() variant of the test, where
  // the test cannot see how much space the IoHandle would have reserved.
  static constexpr uint64_t kFullReadSize = 16384;

  NiceMock<MockIoHandle> io_handle_;
  NiceMock<MockTransportSocketCallbacks> callbacks_;
  RawBufferSocket socket_;
  Buffer::OwnedImpl buffer_;
};

INSTANTIATE_TEST_SUITE_P(StopOnShortRead, RawBufferSocketReadTest, testing::Bool());

// A read that returns less than was asked for: with stop-on-short-read this is the only read, and
// otherwise one more read finds that no data is available.
TEST_P(RawBufferSocketReadTest, ShortRead) {
  InSequence s;
  expectDataRead(10);
  if (!stopOnShortRead()) {
    expectRead(again);
  }

  const IoResult result = socket_.doRead(buffer_);
  EXPECT_EQ(PostIoAction::KeepOpen, result.action_);
  EXPECT_EQ(10, result.bytes_processed_);
  EXPECT_FALSE(result.end_stream_read_);
  EXPECT_FALSE(result.err_code_.has_value());
  EXPECT_EQ(10, buffer_.length());
}

// A read that fills its space says nothing about what is left, so another read follows in both
// variants.
TEST_P(RawBufferSocketReadTest, FullReadThenShortRead) {
  InSequence s;
  expectDataRead(kFull);
  expectDataRead(5);
  if (!stopOnShortRead()) {
    expectRead(again);
  }

  const IoResult result = socket_.doRead(buffer_);
  EXPECT_EQ(PostIoAction::KeepOpen, result.action_);
  EXPECT_GT(result.bytes_processed_, 5);
  EXPECT_EQ(buffer_.length(), result.bytes_processed_);
  EXPECT_FALSE(result.end_stream_read_);
}

TEST_P(RawBufferSocketReadTest, FullReadThenNoData) {
  InSequence s;
  expectDataRead(kFull);
  expectRead(again);

  const IoResult result = socket_.doRead(buffer_);
  EXPECT_EQ(PostIoAction::KeepOpen, result.action_);
  EXPECT_EQ(buffer_.length(), result.bytes_processed_);
  EXPECT_FALSE(result.end_stream_read_);
  EXPECT_FALSE(result.err_code_.has_value());
}

TEST_P(RawBufferSocketReadTest, NoData) {
  expectRead(again);

  const IoResult result = socket_.doRead(buffer_);
  EXPECT_EQ(PostIoAction::KeepOpen, result.action_);
  EXPECT_EQ(0, result.bytes_processed_);
  EXPECT_FALSE(result.end_stream_read_);
  EXPECT_FALSE(result.err_code_.has_value());
}

// The end of the stream and an error are reported the same way in both variants when the read
// that returns them is made.
TEST_P(RawBufferSocketReadTest, EndOfStream) {
  expectRead(endOfStream);

  const IoResult result = socket_.doRead(buffer_);
  EXPECT_EQ(PostIoAction::KeepOpen, result.action_);
  EXPECT_EQ(0, result.bytes_processed_);
  EXPECT_TRUE(result.end_stream_read_);
}

TEST_P(RawBufferSocketReadTest, FullReadThenEndOfStream) {
  InSequence s;
  expectDataRead(kFull);
  expectRead(endOfStream);

  const IoResult result = socket_.doRead(buffer_);
  EXPECT_EQ(PostIoAction::KeepOpen, result.action_);
  EXPECT_EQ(buffer_.length(), result.bytes_processed_);
  EXPECT_TRUE(result.end_stream_read_);
}

TEST_P(RawBufferSocketReadTest, FullReadThenConnectionReset) {
  InSequence s;
  expectDataRead(kFull);
  expectRead(connectionReset);

  const IoResult result = socket_.doRead(buffer_);
  EXPECT_EQ(PostIoAction::Close, result.action_);
  EXPECT_EQ(buffer_.length(), result.bytes_processed_);
  EXPECT_FALSE(result.end_stream_read_);
  ASSERT_TRUE(result.err_code_.has_value());
  EXPECT_EQ(Api::IoError::IoErrorCode::ConnectionReset, result.err_code_.value());
}

// Yielding because of the read buffer limit comes first: the socket asks to be read again, also
// after a short read.
TEST_P(RawBufferSocketReadTest, ShortReadOverReadBufferLimit) {
  expectDataRead(10);
  EXPECT_CALL(callbacks_, shouldDrainReadBuffer()).WillOnce(Return(true));
  EXPECT_CALL(callbacks_, setTransportSocketIsReadable());

  const IoResult result = socket_.doRead(buffer_);
  EXPECT_EQ(PostIoAction::KeepOpen, result.action_);
  EXPECT_EQ(10, result.bytes_processed_);
  EXPECT_FALSE(result.end_stream_read_);
}

} // namespace Network
} // namespace Envoy
