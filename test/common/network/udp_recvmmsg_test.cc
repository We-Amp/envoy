#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "envoy/common/exception.h"
#include "envoy/common/platform.h"
#include "envoy/common/time.h"

#include "source/common/api/os_sys_calls_impl.h"
#include "source/common/memory/stats.h"
#include "source/common/network/address_impl.h"
#include "source/common/network/io_socket_handle_impl.h"
#include "source/common/network/utility.h"

#include "test/test_common/threadsafe_singleton_injector.h"

#include "gtest/gtest.h"

// The tests in this file count the recvmmsg system calls, the receive buffers and (in an ASAN
// build) the heap allocations of a UDP read event. recvmmsg only exists on Linux.
#if defined(__linux__)

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define UDP_RECVMMSG_TEST_COUNTS_ALLOCATIONS
#endif
#endif

#ifdef UDP_RECVMMSG_TEST_COUNTS_ALLOCATIONS
#include <sanitizer/allocator_interface.h>
#endif

namespace Envoy {
namespace Network {
namespace {

// The size of a UDP datagram of a QUIC connection.
constexpr uint32_t QuicDatagramSize = 1200;

// Counts the heap allocations made between start() and stop(). Only an ASAN build can do that:
// its allocator calls a hook for every allocation.
class AllocationCounter {
public:
  struct Counts {
    uint64_t allocations_{0};
    uint64_t bytes_{0};
  };

  static bool available() {
#ifdef UDP_RECVMMSG_TEST_COUNTS_ALLOCATIONS
    static const bool installed =
        __sanitizer_install_malloc_and_free_hooks(&AllocationCounter::onMalloc,
                                                  &AllocationCounter::onFree) != 0;
    return installed;
#else
    return false;
#endif
  }

  static void start() {
    allocations_ = 0;
    bytes_ = 0;
    counting_ = true;
  }

  static Counts stop() {
    counting_ = false;
    return {allocations_, bytes_};
  }

private:
  static void onMalloc(const volatile void*, size_t size) {
    if (counting_) {
      allocations_++;
      bytes_ += size;
    }
  }
  static void onFree(const volatile void*) {}

  static std::atomic<bool> counting_;
  static std::atomic<uint64_t> allocations_;
  static std::atomic<uint64_t> bytes_;
};

std::atomic<bool> AllocationCounter::counting_{false};
std::atomic<uint64_t> AllocationCounter::allocations_{0};
std::atomic<uint64_t> AllocationCounter::bytes_{0};

// One datagram that a scripted receive call returns.
struct ScriptedDatagram {
  // The length of the datagram. A datagram that is longer than the receive buffer is truncated.
  uint32_t length_{0};
  // Every byte of the payload.
  char fill_{'a'};
  // If not negative, the datagram carries the IP_TOS control message with this value.
  int tos_{-1};
  // If not negative, the datagram carries the SO_RXQ_OVFL control message with this value.
  int64_t dropped_{-1};
  // Whether the datagram carries the IP_PKTINFO control message.
  bool pktinfo_{false};
};

// One scripted receive call: an error, or the datagrams that are in the receive queue.
struct ScriptedCall {
  int errno_{0};
  std::vector<ScriptedDatagram> datagrams_;
};

// What one recvmmsg call was asked for and what it returned.
struct RecvMmsgCall {
  unsigned int vlen_{0};
  int rc_{0};
  std::array<void*, NUM_DATAGRAMS_PER_RECEIVE> base_{};
  std::array<size_t, NUM_DATAGRAMS_PER_RECEIVE> len_{};
};

// Replaces the receive system calls with a script, and records the calls. It does not allocate
// while it runs, so that the allocations of the code under test can be counted.
class ScriptedUdpSysCalls : public Api::OsSysCallsImpl {
public:
  ScriptedUdpSysCalls() { recvmmsg_calls_.reserve(64); }

  void expect(std::vector<ScriptedDatagram> datagrams) {
    script_.push_back(ScriptedCall{0, std::move(datagrams)});
  }
  void expectError(int error) { script_.push_back(ScriptedCall{error, {}}); }
  static std::vector<ScriptedDatagram> datagrams(uint32_t count, uint32_t length, char first) {
    std::vector<ScriptedDatagram> result;
    result.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
      result.push_back(ScriptedDatagram{length, static_cast<char>(first + i)});
    }
    return result;
  }
  bool scriptDone() const { return next_ == script_.size(); }
  void reset() {
    script_.clear();
    next_ = 0;
    next_datagram_ = 0;
    recvmmsg_calls_.clear();
    recvmsg_calls_ = 0;
  }

  // Api::OsSysCalls
  Api::SysCallIntResult recvmmsg(os_fd_t, struct mmsghdr* msgvec, unsigned int vlen, int,
                                 struct timespec*) override {
    if (on_recvmmsg_ != nullptr) {
      on_recvmmsg_();
    }
    RecvMmsgCall call;
    call.vlen_ = vlen;
    for (unsigned int i = 0; i < vlen && i < NUM_DATAGRAMS_PER_RECEIVE; i++) {
      call.base_[i] = msgvec[i].msg_hdr.msg_iov[0].iov_base;
      call.len_[i] = msgvec[i].msg_hdr.msg_iov[0].iov_len;
    }
    if (next_ == script_.size()) {
      call.rc_ = -1;
      record(call);
      return {-1, SOCKET_ERROR_AGAIN};
    }
    const ScriptedCall& step = script_[next_++];
    if (step.errno_ != 0) {
      call.rc_ = -1;
      record(call);
      return {-1, step.errno_};
    }
    RELEASE_ASSERT(step.datagrams_.size() <= vlen, "the script returns more than was asked for");
    for (size_t i = 0; i < step.datagrams_.size(); i++) {
      msgvec[i].msg_len = fill(msgvec[i].msg_hdr, step.datagrams_[i]);
    }
    call.rc_ = step.datagrams_.size();
    record(call);
    return {call.rc_, 0};
  }

  Api::SysCallSizeResult recvmsg(os_fd_t, msghdr* msg, int) override {
    recvmsg_calls_++;
    while (next_ < script_.size() && script_[next_].errno_ == 0 &&
           next_datagram_ == script_[next_].datagrams_.size()) {
      next_++;
      next_datagram_ = 0;
    }
    if (next_ == script_.size()) {
      return {-1, SOCKET_ERROR_AGAIN};
    }
    if (script_[next_].errno_ != 0) {
      return {-1, script_[next_++].errno_};
    }
    return {static_cast<ssize_t>(fill(*msg, script_[next_].datagrams_[next_datagram_++])), 0};
  }

  bool supportsMmsg() const override { return true; }
  bool supportsUdpGro() const override { return supports_gro_; }

  std::vector<RecvMmsgCall> recvmmsg_calls_;
  uint32_t recvmsg_calls_{0};
  // Called at the start of every recvmmsg call.
  std::function<void()> on_recvmmsg_;
  // Whether the platform reads with generic receive offload when a socket asks for it.
  bool supports_gro_{false};

private:
  void record(const RecvMmsgCall& call) {
    RELEASE_ASSERT(recvmmsg_calls_.size() < recvmmsg_calls_.capacity(), "too many calls");
    recvmmsg_calls_.push_back(call);
  }

  // Fills a message the way the kernel does, and returns the length of the datagram.
  static uint32_t fill(msghdr& hdr, const ScriptedDatagram& datagram) {
    const size_t copied = std::min<size_t>(datagram.length_, hdr.msg_iov[0].iov_len);
    memset(hdr.msg_iov[0].iov_base, datagram.fill_, copied);
    hdr.msg_flags = datagram.length_ > hdr.msg_iov[0].iov_len ? MSG_TRUNC : 0;

    sockaddr_in peer_address{};
    peer_address.sin_family = AF_INET;
    peer_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    peer_address.sin_port = htons(12345);
    memcpy(hdr.msg_name, &peer_address, sizeof(peer_address));
    hdr.msg_namelen = sizeof(peer_address);

    const size_t control_space = hdr.msg_controllen;
    size_t control_used = 0;
    char* const control = static_cast<char*>(hdr.msg_control);
    const auto add = [&](int level, int type, const void* data, size_t size) {
      RELEASE_ASSERT(control_used + CMSG_SPACE(size) <= control_space, "control buffer too small");
      cmsghdr* cmsg = reinterpret_cast<cmsghdr*>(control + control_used);
      cmsg->cmsg_level = level;
      cmsg->cmsg_type = type;
      cmsg->cmsg_len = CMSG_LEN(size);
      memcpy(CMSG_DATA(cmsg), data, size);
      control_used += CMSG_SPACE(size);
    };
    if (datagram.dropped_ >= 0) {
      const uint32_t dropped = datagram.dropped_;
      add(SOL_SOCKET, SO_RXQ_OVFL, &dropped, sizeof(dropped));
    }
    if (datagram.tos_ >= 0) {
      const uint8_t tos = datagram.tos_;
      add(IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
    }
    if (datagram.pktinfo_) {
      in_pktinfo info{};
      info.ipi_addr.s_addr = htonl(INADDR_LOOPBACK);
      add(IPPROTO_IP, IP_PKTINFO, &info, sizeof(info));
    }
    hdr.msg_controllen = control_used;
    return datagram.length_;
  }

  std::vector<ScriptedCall> script_;
  size_t next_{0};
  size_t next_datagram_{0};
};

// A time source whose monotonic time moves by a millisecond with every reading, so that two
// readings never return the same time.
class SteppingTimeSource : public TimeSource {
public:
  SystemTime systemTime() override { return {}; }
  MonotonicTime monotonicTime() override {
    now_ += std::chrono::milliseconds(1);
    return now_;
  }

private:
  MonotonicTime now_;
};

// Keeps every packet it is given, with the buffer that carried it.
class KeepingPacketProcessor : public UdpPacketProcessor {
public:
  struct Packet {
    Buffer::InstancePtr buffer_;
    const void* mem_{nullptr};
    uint64_t length_{0};
    MonotonicTime receive_time_;
    uint8_t tos_{0};
    bool has_local_address_{false};
    bool has_peer_address_{false};
  };

  KeepingPacketProcessor() { packets_.reserve(256); }

  // Network::UdpPacketProcessor
  void processPacket(Address::InstanceConstSharedPtr local_address,
                     Address::InstanceConstSharedPtr peer_address, Buffer::InstancePtr buffer,
                     MonotonicTime receive_time, uint8_t tos, Buffer::OwnedImpl) override {
    RELEASE_ASSERT(packets_.size() < packets_.capacity(), "too many packets");
    Packet packet;
    packet.mem_ = buffer->frontSlice().mem_;
    packet.length_ = buffer->length();
    packet.receive_time_ = receive_time;
    packet.tos_ = tos;
    packet.has_local_address_ = local_address != nullptr;
    packet.has_peer_address_ = peer_address != nullptr;
    packet.buffer_ = std::move(buffer);
    packets_.push_back(std::move(packet));
    if (on_packet_ != nullptr) {
      on_packet_();
    }
  }
  void onDatagramsDropped(uint32_t dropped) override {
    dropped_ += dropped;
    dropped_calls_++;
  }
  uint64_t maxDatagramSize() const override { return max_datagram_size_; }
  size_t numPacketsExpectedPerEventLoop() const override { return packets_expected_; }
  const IoHandle::UdpSaveCmsgConfig& saveCmsgConfig() const override { return save_cmsg_config_; }
  UdpRecvMmsgBuffers* recvMmsgBuffers() override { return recv_mmsg_buffers_.get(); }

  // The number of receive buffers that were allocated to be kept between calls.
  uint64_t keptBuffersAllocated() const {
    return recv_mmsg_buffers_ == nullptr
               ? 0
               : Utility::udpRecvMmsgBuffersAllocatedForTest(*recv_mmsg_buffers_);
  }

  // The payload of a packet, which must be one run of the same byte.
  std::string payload(size_t index) const {
    std::string payload = packets_[index].buffer_->toString();
    EXPECT_EQ(std::string(payload.size(), payload.empty() ? ' ' : payload[0]), payload);
    return payload;
  }

  std::vector<Packet> packets_;
  uint32_t dropped_{0};
  uint32_t dropped_calls_{0};
  uint64_t max_datagram_size_{DEFAULT_UDP_MAX_DATAGRAM_SIZE};
  size_t packets_expected_{32};
  IoHandle::UdpSaveCmsgConfig save_cmsg_config_;
  UdpRecvMmsgBuffersPtr recv_mmsg_buffers_;
  // Called after a packet was taken.
  std::function<void()> on_packet_;
};

// Drives Utility::readPacketsFromSocket() against scripted system calls. The parameter is whether
// the packet processor has receive buffers that are kept between calls.
class UdpRecvMmsgTest : public testing::TestWithParam<bool> {
public:
  UdpRecvMmsgTest() {
    if (reuse()) {
      processor_.recv_mmsg_buffers_ = Utility::createUdpRecvMmsgBuffers();
    }
  }

  bool reuse() const { return GetParam(); }

  static std::string paramName(const testing::TestParamInfo<bool>& info) {
    return info.param ? "ReuseOn" : "ReuseOff";
  }

  // One read event of a socket.
  Api::IoErrorPtr readEvent() {
    return Utility::readPacketsFromSocket(handle_, local_address_, processor_, time_source_,
                                          allow_gro_, allow_mmsg_, packets_dropped_);
  }

  void expectAgain(const Api::IoErrorPtr& error) {
    ASSERT_NE(nullptr, error);
    EXPECT_EQ(Api::IoError::IoErrorCode::Again, error->getErrorCode());
  }

  size_t recvmmsgCalls() const { return sys_calls_.recvmmsg_calls_.size(); }

  ScriptedUdpSysCalls sys_calls_;
  TestThreadsafeSingletonInjector<Api::OsSysCallsImpl> os_calls_{&sys_calls_};
  IoSocketHandleImpl handle_;
  const Address::Ipv4Instance local_address_{"127.0.0.1", 4433};
  KeepingPacketProcessor processor_;
  SteppingTimeSource time_source_;
  bool allow_gro_{false};
  bool allow_mmsg_{true};
  uint32_t packets_dropped_{0};
  std::vector<size_t> packets_before_call_;
};

INSTANTIATE_TEST_SUITE_P(KeptBuffers, UdpRecvMmsgTest, testing::Bool(), UdpRecvMmsgTest::paramName);

// The common case of a QUIC listener at low load: one datagram is in the receive queue.
TEST_P(UdpRecvMmsgTest, OneDatagram) {
  sys_calls_.expect({{100, 'a'}});

  expectAgain(readEvent());

  // A second call is made, which finds nothing.
  EXPECT_EQ(2, recvmmsgCalls());
  EXPECT_EQ(NUM_DATAGRAMS_PER_RECEIVE, sys_calls_.recvmmsg_calls_[0].vlen_);
  EXPECT_EQ(1, sys_calls_.recvmmsg_calls_[0].rc_);
  EXPECT_EQ(-1, sys_calls_.recvmmsg_calls_[1].rc_);
  ASSERT_EQ(1, processor_.packets_.size());
  EXPECT_EQ(std::string(100, 'a'), processor_.payload(0));
  EXPECT_TRUE(processor_.packets_[0].has_peer_address_);
  EXPECT_EQ(0, processor_.dropped_calls_);
}

// Every call is given a full set of receive buffers, each of the configured datagram size.
TEST_P(UdpRecvMmsgTest, ReceiveBuffersOfACall) {
  sys_calls_.expect({{100, 'a'}});

  expectAgain(readEvent());

  for (const RecvMmsgCall& call : sys_calls_.recvmmsg_calls_) {
    ASSERT_EQ(NUM_DATAGRAMS_PER_RECEIVE, call.vlen_);
    for (size_t i = 0; i < NUM_DATAGRAMS_PER_RECEIVE; i++) {
      EXPECT_NE(nullptr, call.base_[i]);
      EXPECT_EQ(DEFAULT_UDP_MAX_DATAGRAM_SIZE, call.len_[i]);
      for (size_t j = 0; j < i; j++) {
        EXPECT_NE(call.base_[j], call.base_[i]);
      }
    }
  }
  // The packet is handed over in the buffer that the kernel wrote to.
  EXPECT_EQ(sys_calls_.recvmmsg_calls_[0].base_[0], processor_.packets_[0].mem_);
}

TEST_P(UdpRecvMmsgTest, ShortBatch) {
  sys_calls_.expect(ScriptedUdpSysCalls::datagrams(NUM_DATAGRAMS_PER_RECEIVE - 1, 10, 'a'));

  expectAgain(readEvent());

  EXPECT_EQ(2, recvmmsgCalls());
  ASSERT_EQ(NUM_DATAGRAMS_PER_RECEIVE - 1, processor_.packets_.size());
  for (size_t i = 0; i < processor_.packets_.size(); i++) {
    EXPECT_EQ(std::string(10, 'a' + i), processor_.payload(i));
  }
}

// A batch that fills every receive buffer says nothing about the receive queue: another call is
// always made.
TEST_P(UdpRecvMmsgTest, FullBatchThenNothing) {
  sys_calls_.expect(ScriptedUdpSysCalls::datagrams(NUM_DATAGRAMS_PER_RECEIVE, 10, 'a'));

  expectAgain(readEvent());

  EXPECT_EQ(2, recvmmsgCalls());
  EXPECT_EQ(-1, sys_calls_.recvmmsg_calls_[1].rc_);
  EXPECT_EQ(NUM_DATAGRAMS_PER_RECEIVE, processor_.packets_.size());
}

TEST_P(UdpRecvMmsgTest, FullBatchThenShortBatch) {
  sys_calls_.expect(ScriptedUdpSysCalls::datagrams(NUM_DATAGRAMS_PER_RECEIVE, 10, 'a'));
  sys_calls_.expect(ScriptedUdpSysCalls::datagrams(3, 20, 'A'));

  expectAgain(readEvent());

  EXPECT_EQ(3, recvmmsgCalls());
  ASSERT_EQ(NUM_DATAGRAMS_PER_RECEIVE + 3, processor_.packets_.size());
  // Order and content of the datagrams.
  for (size_t i = 0; i < NUM_DATAGRAMS_PER_RECEIVE; i++) {
    EXPECT_EQ(std::string(10, 'a' + i), processor_.payload(i));
  }
  for (size_t i = 0; i < 3; i++) {
    EXPECT_EQ(std::string(20, 'A' + i), processor_.payload(NUM_DATAGRAMS_PER_RECEIVE + i));
  }
  // One receive time for each batch.
  for (size_t i = 1; i < NUM_DATAGRAMS_PER_RECEIVE; i++) {
    EXPECT_EQ(processor_.packets_[0].receive_time_, processor_.packets_[i].receive_time_);
  }
  EXPECT_LT(processor_.packets_[0].receive_time_,
            processor_.packets_[NUM_DATAGRAMS_PER_RECEIVE].receive_time_);
  EXPECT_EQ(processor_.packets_[NUM_DATAGRAMS_PER_RECEIVE].receive_time_,
            processor_.packets_[NUM_DATAGRAMS_PER_RECEIVE + 2].receive_time_);
}

// The number of packets to read in one event is not changed by kept buffers. When it is used up,
// the result is "no error", which makes the caller schedule another read.
TEST_P(UdpRecvMmsgTest, PacketsPerEventUsedUp) {
  processor_.packets_expected_ = 1;
  sys_calls_.expect({{10, 'a'}});
  EXPECT_EQ(nullptr, readEvent());
  EXPECT_EQ(1, recvmmsgCalls());

  // The read that the caller schedules finds nothing.
  expectAgain(readEvent());
  EXPECT_EQ(2, recvmmsgCalls());
  EXPECT_EQ(1, processor_.packets_.size());
}

TEST_P(UdpRecvMmsgTest, NoPacketsExpected) {
  processor_.packets_expected_ = 0;
  sys_calls_.expect({{10, 'a'}});
  EXPECT_EQ(nullptr, readEvent());
  EXPECT_EQ(1, recvmmsgCalls());
  EXPECT_EQ(1, processor_.packets_.size());
}

TEST_P(UdpRecvMmsgTest, PacketsPerEventUsedUpByAFullBatch) {
  processor_.packets_expected_ = NUM_DATAGRAMS_PER_RECEIVE;
  sys_calls_.expect(ScriptedUdpSysCalls::datagrams(NUM_DATAGRAMS_PER_RECEIVE, 10, 'a'));
  sys_calls_.expect({{10, 'z'}});
  EXPECT_EQ(nullptr, readEvent());
  EXPECT_EQ(1, recvmmsgCalls());
  EXPECT_EQ(NUM_DATAGRAMS_PER_RECEIVE, processor_.packets_.size());
}

TEST_P(UdpRecvMmsgTest, PacketsPerEventUsedUpByAShortBatch) {
  processor_.packets_expected_ = NUM_DATAGRAMS_PER_RECEIVE + 4;
  sys_calls_.expect(ScriptedUdpSysCalls::datagrams(NUM_DATAGRAMS_PER_RECEIVE, 10, 'a'));
  sys_calls_.expect(ScriptedUdpSysCalls::datagrams(4, 10, 'A'));
  EXPECT_EQ(nullptr, readEvent());
  EXPECT_EQ(2, recvmmsgCalls());
  EXPECT_EQ(NUM_DATAGRAMS_PER_RECEIVE + 4, processor_.packets_.size());
}

TEST_P(UdpRecvMmsgTest, ShortBatchBelowPacketsPerEvent) {
  processor_.packets_expected_ = NUM_DATAGRAMS_PER_RECEIVE + 4;
  sys_calls_.expect(ScriptedUdpSysCalls::datagrams(NUM_DATAGRAMS_PER_RECEIVE, 10, 'a'));
  sys_calls_.expect(ScriptedUdpSysCalls::datagrams(3, 10, 'A'));
  expectAgain(readEvent());
  EXPECT_EQ(3, recvmmsgCalls());
  EXPECT_EQ(NUM_DATAGRAMS_PER_RECEIVE + 3, processor_.packets_.size());
}

// A datagram that does not fit in the receive buffer is dropped and counted, and it counts as a
// datagram that the kernel returned.
TEST_P(UdpRecvMmsgTest, TruncatedDatagramInAShortBatch) {
  sys_calls_.expect({{10, 'a'}, {5000, 'x'}, {20, 'b'}});

  expectAgain(readEvent());

  EXPECT_EQ(2, recvmmsgCalls());
  EXPECT_EQ(3, sys_calls_.recvmmsg_calls_[0].rc_);
  ASSERT_EQ(2, processor_.packets_.size());
  EXPECT_EQ(std::string(10, 'a'), processor_.payload(0));
  EXPECT_EQ(std::string(20, 'b'), processor_.payload(1));
  EXPECT_EQ(1, packets_dropped_);
  EXPECT_EQ(1, processor_.dropped_);
  EXPECT_EQ(1, processor_.dropped_calls_);
}

// The kernel filled every receive buffer, although one datagram is dropped: that is a full batch.
TEST_P(UdpRecvMmsgTest, TruncatedDatagramInAFullBatch) {
  std::vector<ScriptedDatagram> datagrams =
      ScriptedUdpSysCalls::datagrams(NUM_DATAGRAMS_PER_RECEIVE, 10, 'a');
  datagrams[5].length_ = 5000;
  sys_calls_.expect(datagrams);
  sys_calls_.expect({{10, 'z'}});

  expectAgain(readEvent());

  EXPECT_EQ(3, recvmmsgCalls());
  ASSERT_EQ(NUM_DATAGRAMS_PER_RECEIVE, processor_.packets_.size());
  EXPECT_EQ(std::string(10, 'z'), processor_.payload(NUM_DATAGRAMS_PER_RECEIVE - 1));
  EXPECT_EQ(1, processor_.dropped_);
}

TEST_P(UdpRecvMmsgTest, OnlyTruncatedDatagrams) {
  sys_calls_.expect({{5000, 'x'}, {6000, 'y'}});

  expectAgain(readEvent());

  EXPECT_EQ(2, recvmmsgCalls());
  EXPECT_EQ(0, processor_.packets_.size());
  EXPECT_EQ(2, processor_.dropped_);
}

TEST_P(UdpRecvMmsgTest, ZeroLengthDatagram) {
  sys_calls_.expect({{0, 'a'}, {5, 'b'}});

  expectAgain(readEvent());

  EXPECT_EQ(2, recvmmsgCalls());
  ASSERT_EQ(2, processor_.packets_.size());
  EXPECT_EQ(0, processor_.packets_[0].length_);
  EXPECT_EQ(std::string(5, 'b'), processor_.payload(1));
}

// The count of datagrams that the kernel dropped for lack of receive buffer space is taken from
// the datagrams that are read, with or without kept buffers.
TEST_P(UdpRecvMmsgTest, KernelDropCounter) {
  ScriptedDatagram datagram{10, 'a'};
  datagram.dropped_ = 7;
  sys_calls_.expect({datagram});
  expectAgain(readEvent());
  EXPECT_EQ(7, packets_dropped_);
  EXPECT_EQ(7, processor_.dropped_);
  EXPECT_EQ(1, processor_.dropped_calls_);

  sys_calls_.expect({datagram});
  expectAgain(readEvent());
  EXPECT_EQ(7, processor_.dropped_);
  EXPECT_EQ(1, processor_.dropped_calls_);

  datagram.dropped_ = 9;
  sys_calls_.expect({datagram, {10, 'b'}});
  expectAgain(readEvent());
  EXPECT_EQ(9, packets_dropped_);
  EXPECT_EQ(9, processor_.dropped_);
  EXPECT_EQ(2, processor_.dropped_calls_);
  EXPECT_EQ(4, processor_.packets_.size());
}

TEST_P(UdpRecvMmsgTest, ErrorOnTheFirstCall) {
  sys_calls_.expectError(SOCKET_ERROR_CONNRESET);

  const Api::IoErrorPtr error = readEvent();

  ASSERT_NE(nullptr, error);
  EXPECT_EQ(Api::IoError::IoErrorCode::ConnectionReset, error->getErrorCode());
  EXPECT_EQ(1, recvmmsgCalls());
}

TEST_P(UdpRecvMmsgTest, ErrorAfterAFullBatch) {
  sys_calls_.expect(ScriptedUdpSysCalls::datagrams(NUM_DATAGRAMS_PER_RECEIVE, 10, 'a'));
  sys_calls_.expectError(SOCKET_ERROR_CONNRESET);

  const Api::IoErrorPtr error = readEvent();

  ASSERT_NE(nullptr, error);
  EXPECT_EQ(Api::IoError::IoErrorCode::ConnectionReset, error->getErrorCode());
  EXPECT_EQ(2, recvmmsgCalls());
  EXPECT_EQ(NUM_DATAGRAMS_PER_RECEIVE, processor_.packets_.size());
}

// The kernel ends a batch early when a receive call fails after the first datagram, and keeps the
// error for the next call, which is made in the same read event. The buffers that the short batch
// did not fill are kept for the call after the error.
TEST_P(UdpRecvMmsgTest, ErrorAfterAShortBatch) {
  sys_calls_.expect({{10, 'a'}, {10, 'b'}});
  sys_calls_.expectError(SOCKET_ERROR_CONNRESET);

  const Api::IoErrorPtr error = readEvent();

  ASSERT_NE(nullptr, error);
  EXPECT_EQ(Api::IoError::IoErrorCode::ConnectionReset, error->getErrorCode());
  EXPECT_EQ(2, recvmmsgCalls());
  EXPECT_TRUE(sys_calls_.scriptDone());
  ASSERT_EQ(2, processor_.packets_.size());
  EXPECT_EQ(std::string(10, 'a'), processor_.payload(0));
  EXPECT_EQ(std::string(10, 'b'), processor_.payload(1));

  sys_calls_.expect({{20, 'c'}});
  expectAgain(readEvent());
  ASSERT_EQ(3, processor_.packets_.size());
  EXPECT_EQ(std::string(20, 'c'), processor_.payload(2));
  if (reuse()) {
    // The two buffers that were handed over were replaced once, before the call that failed.
    // After that only the one of the third datagram was.
    const std::vector<RecvMmsgCall>& calls = sys_calls_.recvmmsg_calls_;
    EXPECT_EQ(NUM_DATAGRAMS_PER_RECEIVE + 3, processor_.keptBuffersAllocated());
    for (size_t i = 1; i < calls.size(); i++) {
      for (size_t j = 2; j < NUM_DATAGRAMS_PER_RECEIVE; j++) {
        EXPECT_EQ(calls[0].base_[j], calls[i].base_[j]);
      }
    }
    EXPECT_EQ(calls[1].base_[1], calls[2].base_[1]);
  }
}

// The receive buffers that the kernel did not fill are offered to the next call, and only the
// ones that were handed over are replaced.
TEST_P(UdpRecvMmsgTest, UnusedReceiveBuffersAreKept) {
  constexpr uint32_t events = 5;
  for (uint32_t i = 0; i < events; i++) {
    sys_calls_.expect({{QuicDatagramSize, static_cast<char>('a' + i)}});
    expectAgain(readEvent());
  }
  ASSERT_EQ(events, processor_.packets_.size());
  const std::vector<RecvMmsgCall>& calls = sys_calls_.recvmmsg_calls_;
  ASSERT_EQ(2 * events, calls.size());
  if (!reuse()) {
    EXPECT_EQ(0, processor_.keptBuffersAllocated());
    return;
  }
  // The first call allocates all of them. After that, one is allocated for every packet that was
  // handed over, at the start of the call that follows.
  uint32_t packets_before_last_call = 0;
  for (size_t i = 0; i + 1 < calls.size(); i++) {
    packets_before_last_call += calls[i].rc_ > 0 ? calls[i].rc_ : 0;
  }
  EXPECT_EQ(NUM_DATAGRAMS_PER_RECEIVE + packets_before_last_call,
            processor_.keptBuffersAllocated());
  for (size_t i = 1; i < calls.size(); i++) {
    // The buffers that no call filled are the same in every call.
    for (size_t j = 1; j < NUM_DATAGRAMS_PER_RECEIVE; j++) {
      EXPECT_EQ(calls[0].base_[j], calls[i].base_[j]);
      EXPECT_EQ(DEFAULT_UDP_MAX_DATAGRAM_SIZE, calls[i].len_[j]);
    }
    EXPECT_EQ(DEFAULT_UDP_MAX_DATAGRAM_SIZE, calls[i].len_[0]);
  }
  for (uint32_t i = 0; i < events; i++) {
    EXPECT_EQ(std::string(QuicDatagramSize, 'a' + i), processor_.payload(i));
  }
}

// A buffer that was handed to the processor belongs to the processor. The test keeps all of them
// alive, so none of their memory can be offered to the kernel again unless it is still referenced.
TEST_P(UdpRecvMmsgTest, HandedOverBuffersAreNotOfferedAgain) {
  sys_calls_.on_recvmmsg_ = [this]() {
    packets_before_call_.push_back(processor_.packets_.size());
  };
  processor_.packets_expected_ = 1000;
  const std::vector<uint32_t> batches = {
      3, NUM_DATAGRAMS_PER_RECEIVE, 1, NUM_DATAGRAMS_PER_RECEIVE, NUM_DATAGRAMS_PER_RECEIVE, 5, 1,
      1};
  uint32_t total = 0;
  for (const uint32_t batch : batches) {
    sys_calls_.expect(ScriptedUdpSysCalls::datagrams(batch, 100 + batch, 'a'));
    total += batch;
    if (batch < NUM_DATAGRAMS_PER_RECEIVE) {
      expectAgain(readEvent());
    }
  }
  ASSERT_TRUE(sys_calls_.scriptDone());
  ASSERT_EQ(total, processor_.packets_.size());
  const std::vector<RecvMmsgCall>& calls = sys_calls_.recvmmsg_calls_;
  ASSERT_EQ(calls.size(), packets_before_call_.size());
  for (size_t i = 0; i < calls.size(); i++) {
    for (size_t j = 0; j < NUM_DATAGRAMS_PER_RECEIVE; j++) {
      for (size_t k = 0; k < packets_before_call_[i]; k++) {
        ASSERT_NE(calls[i].base_[j], processor_.packets_[k].mem_)
            << "call " << i << " offers the memory of packet " << k;
      }
    }
  }
  // Every packet has the content that the kernel wrote for it.
  size_t packet = 0;
  for (const uint32_t batch : batches) {
    for (uint32_t i = 0; i < batch; i++) {
      EXPECT_EQ(std::string(100 + batch, 'a' + i), processor_.payload(packet++));
    }
  }
  if (reuse()) {
    EXPECT_GE(processor_.keptBuffersAllocated(), NUM_DATAGRAMS_PER_RECEIVE + total - 1);
    EXPECT_LE(processor_.keptBuffersAllocated(), NUM_DATAGRAMS_PER_RECEIVE + total);
  }
}

// The buffer of a datagram that was truncated is not handed over. It is used for a later
// datagram, of which only the bytes that belong to it are handed over.
TEST_P(UdpRecvMmsgTest, BufferOfATruncatedDatagramIsUsedAgain) {
  sys_calls_.expect({{5000, 'x'}});
  expectAgain(readEvent());
  EXPECT_EQ(0, processor_.packets_.size());
  EXPECT_EQ(1, processor_.dropped_);

  sys_calls_.expect({{10, 'y'}});
  expectAgain(readEvent());
  ASSERT_EQ(1, processor_.packets_.size());
  EXPECT_EQ(std::string(10, 'y'), processor_.payload(0));
  EXPECT_EQ(1, processor_.dropped_);
  if (reuse()) {
    const RecvMmsgCall& first = sys_calls_.recvmmsg_calls_[0];
    const RecvMmsgCall& second = sys_calls_.recvmmsg_calls_[2];
    ASSERT_EQ(1, second.rc_);
    EXPECT_EQ(first.base_[0], second.base_[0]);
    EXPECT_EQ(second.base_[0], processor_.packets_[0].mem_);
    EXPECT_EQ(NUM_DATAGRAMS_PER_RECEIVE + 1, processor_.keptBuffersAllocated());
  }
}

// Nothing of a packet is seen by the packet that the next call returns in the same position: not
// the type of service, not the local address, not the truncation.
TEST_P(UdpRecvMmsgTest, PacketInformationIsNotCarriedOver) {
  ScriptedDatagram first{10, 'a'};
  first.tos_ = 3;
  first.pktinfo_ = true;
  sys_calls_.expect({first, {5000, 'x'}});
  expectAgain(readEvent());
  ASSERT_EQ(1, processor_.packets_.size());
  EXPECT_EQ(3, processor_.packets_[0].tos_);
  EXPECT_TRUE(processor_.packets_[0].has_local_address_);
  EXPECT_EQ(1, processor_.dropped_);

  sys_calls_.expect({{20, 'b'}, {30, 'c'}});
  expectAgain(readEvent());
  ASSERT_EQ(3, processor_.packets_.size());
  for (size_t i = 1; i < 3; i++) {
    EXPECT_EQ(0, processor_.packets_[i].tos_);
    EXPECT_FALSE(processor_.packets_[i].has_local_address_);
    EXPECT_TRUE(processor_.packets_[i].has_peer_address_);
  }
  EXPECT_EQ(std::string(20, 'b'), processor_.payload(1));
  EXPECT_EQ(std::string(30, 'c'), processor_.payload(2));
  EXPECT_EQ(1, processor_.dropped_);
}

// A change of the datagram size replaces the kept buffers. Buffers above a size limit are not
// kept: every call allocates them, as it does without kept buffers.
TEST_P(UdpRecvMmsgTest, DatagramSizeChange) {
  sys_calls_.expect({{10, 'a'}});
  expectAgain(readEvent());
  const size_t calls_small = recvmmsgCalls();
  const uint64_t allocated_small = processor_.keptBuffersAllocated();

  processor_.max_datagram_size_ = 3000;
  sys_calls_.expect({{2500, 'b'}});
  expectAgain(readEvent());
  ASSERT_EQ(2, processor_.packets_.size());
  EXPECT_EQ(std::string(2500, 'b'), processor_.payload(1));
  for (size_t i = calls_small; i < recvmmsgCalls(); i++) {
    for (size_t j = 0; j < NUM_DATAGRAMS_PER_RECEIVE; j++) {
      EXPECT_EQ(3000, sys_calls_.recvmmsg_calls_[i].len_[j]);
    }
  }
  const uint64_t allocated_medium = processor_.keptBuffersAllocated();
  if (reuse()) {
    EXPECT_EQ(NUM_DATAGRAMS_PER_RECEIVE + 1, allocated_small);
    EXPECT_EQ(allocated_small + NUM_DATAGRAMS_PER_RECEIVE + 1, allocated_medium);
  }

  const size_t calls_medium = recvmmsgCalls();
  processor_.max_datagram_size_ = 64 * 1024;
  sys_calls_.expect({{40000, 'c'}});
  expectAgain(readEvent());
  ASSERT_EQ(3, processor_.packets_.size());
  EXPECT_EQ(std::string(40000, 'c'), processor_.payload(2));
  for (size_t i = calls_medium; i < recvmmsgCalls(); i++) {
    for (size_t j = 0; j < NUM_DATAGRAMS_PER_RECEIVE; j++) {
      EXPECT_EQ(64 * 1024, sys_calls_.recvmmsg_calls_[i].len_[j]);
    }
  }
  EXPECT_EQ(allocated_medium, processor_.keptBuffersAllocated());

  // Back to a size that is kept.
  processor_.max_datagram_size_ = DEFAULT_UDP_MAX_DATAGRAM_SIZE;
  sys_calls_.expect({{10, 'd'}});
  expectAgain(readEvent());
  ASSERT_EQ(4, processor_.packets_.size());
  EXPECT_EQ(std::string(10, 'd'), processor_.payload(3));
  if (reuse()) {
    EXPECT_EQ(allocated_medium + NUM_DATAGRAMS_PER_RECEIVE + 1, processor_.keptBuffersAllocated());
  }
}

// A call that fails does not use up receive buffers.
TEST_P(UdpRecvMmsgTest, FailedCallKeepsTheReceiveBuffers) {
  sys_calls_.expectError(SOCKET_ERROR_CONNRESET);
  const Api::IoErrorPtr error = readEvent();
  ASSERT_NE(nullptr, error);
  EXPECT_EQ(Api::IoError::IoErrorCode::ConnectionReset, error->getErrorCode());
  expectAgain(readEvent());
  sys_calls_.expect({{10, 'a'}});
  expectAgain(readEvent());
  ASSERT_EQ(1, processor_.packets_.size());
  EXPECT_EQ(std::string(10, 'a'), processor_.payload(0));
  if (reuse()) {
    EXPECT_EQ(NUM_DATAGRAMS_PER_RECEIVE + 1, processor_.keptBuffersAllocated());
    for (const RecvMmsgCall& call : sys_calls_.recvmmsg_calls_) {
      for (size_t j = 1; j < NUM_DATAGRAMS_PER_RECEIVE; j++) {
        EXPECT_EQ(sys_calls_.recvmmsg_calls_[0].base_[j], call.base_[j]);
      }
    }
  }
}

// A read that is started while a packet of another read of the same processor is handled does
// not touch the kept buffers of the read that is in progress.
TEST_P(UdpRecvMmsgTest, ReadFromInsideAPacketCallback) {
  sys_calls_.expect({{10, 'a'}, {10, 'b'}, {10, 'c'}});
  sys_calls_.expect({{20, 'n'}});
  bool nested = false;
  processor_.on_packet_ = [&]() {
    if (!nested) {
      nested = true;
      uint32_t dropped = 0;
      uint32_t read = 0;
      const Api::IoCallUint64Result result =
          Utility::readFromSocket(handle_, local_address_, processor_, time_source_,
                                  UdpRecvMsgMethod::RecvMmsg, &dropped, &read);
      EXPECT_TRUE(result.ok());
      EXPECT_EQ(1, read);
    }
  };

  expectAgain(readEvent());

  ASSERT_EQ(4, processor_.packets_.size());
  EXPECT_EQ(std::string(10, 'a'), processor_.payload(0));
  EXPECT_EQ(std::string(20, 'n'), processor_.payload(1));
  EXPECT_EQ(std::string(10, 'b'), processor_.payload(2));
  EXPECT_EQ(std::string(10, 'c'), processor_.payload(3));
  // The nested call got its own buffers.
  const std::vector<RecvMmsgCall>& calls = sys_calls_.recvmmsg_calls_;
  for (size_t j = 0; j < NUM_DATAGRAMS_PER_RECEIVE; j++) {
    for (size_t k = 0; k < NUM_DATAGRAMS_PER_RECEIVE; k++) {
      EXPECT_NE(calls[0].base_[j], calls[1].base_[k]);
    }
  }
  if (reuse()) {
    EXPECT_LE(processor_.keptBuffersAllocated(), NUM_DATAGRAMS_PER_RECEIVE + 3);
  }
}

// The other ways of reading do not use the kept buffers.
TEST_P(UdpRecvMmsgTest, RecvMsgDoesNotKeepReceiveBuffers) {
  allow_mmsg_ = false;
  sys_calls_.expect({{10, 'a'}});
  expectAgain(readEvent());
  EXPECT_EQ(1, processor_.packets_.size());
  EXPECT_EQ(0, processor_.keptBuffersAllocated());
}

// Reading with recvmsg and generic receive offload does not use the kept buffers either.
TEST_P(UdpRecvMmsgTest, GroDoesNotKeepReceiveBuffers) {
  allow_gro_ = true;
  sys_calls_.supports_gro_ = true;
  sys_calls_.expect({{10, 'a'}});
  expectAgain(readEvent());
  EXPECT_EQ(0, recvmmsgCalls());
  EXPECT_EQ(2, sys_calls_.recvmsg_calls_);
  EXPECT_EQ(1, processor_.packets_.size());
  EXPECT_EQ(0, processor_.keptBuffersAllocated());
}

#ifndef ENVOY_DISABLE_EXCEPTIONS
// The processing of a packet throws. Nothing of the packets of that call that were not handed over
// is seen by the next call: not the truncation, not the type of service, not the local address.
TEST_P(UdpRecvMmsgTest, PacketProcessingThatThrows) {
  ScriptedDatagram truncated{5000, 'x'};
  ScriptedDatagram with_info{10, 'c'};
  with_info.tos_ = 3;
  with_info.pktinfo_ = true;
  sys_calls_.expect({{10, 'a'}, {10, 'b'}, with_info, truncated});
  processor_.on_packet_ = [this]() {
    if (processor_.packets_.size() == 2) {
      throw EnvoyException("packet processing failed");
    }
  };
  EXPECT_THROW(readEvent(), EnvoyException);
  ASSERT_EQ(2, processor_.packets_.size());
  // The truncated datagram was counted by the call, which was not finished.
  EXPECT_EQ(1, packets_dropped_);
  EXPECT_EQ(0, processor_.dropped_);

  processor_.on_packet_ = nullptr;
  sys_calls_.expect({{20, 'd'}, {20, 'e'}, {20, 'f'}, {20, 'g'}});
  expectAgain(readEvent());
  ASSERT_EQ(6, processor_.packets_.size());
  for (size_t i = 2; i < 6; i++) {
    EXPECT_EQ(std::string(20, 'd' + i - 2), processor_.payload(i));
    EXPECT_EQ(0, processor_.packets_[i].tos_);
    EXPECT_FALSE(processor_.packets_[i].has_local_address_);
  }
  // No datagram of the second call is counted as dropped.
  EXPECT_EQ(1, packets_dropped_);
  EXPECT_EQ(0, processor_.dropped_);
}
#endif

// The memory that the kept receive buffers hold while the socket is idle. Counted in an ASAN
// build, and in a build with tcmalloc.
TEST_P(UdpRecvMmsgTest, MemoryHeldByKeptReceiveBuffers) {
  if (!reuse()) {
    GTEST_SKIP() << "nothing is kept";
  }
  // A buffer object and its memory for each, the arrays of slices and of packet information.
  constexpr uint64_t bound = NUM_DATAGRAMS_PER_RECEIVE * (4096 + 2048) + 32768;
  if (AllocationCounter::available()) {
    // The first read of an empty socket creates the buffers and what a call allocates and frees.
    // The second one only does the latter.
    AllocationCounter::start();
    expectAgain(readEvent());
    const AllocationCounter::Counts first = AllocationCounter::stop();
    AllocationCounter::start();
    expectAgain(readEvent());
    const AllocationCounter::Counts second = AllocationCounter::stop();
    EXPECT_EQ(NUM_DATAGRAMS_PER_RECEIVE, processor_.keptBuffersAllocated());
    GTEST_LOG_(INFO) << "kept receive buffers for a datagram size of "
                     << processor_.max_datagram_size_ << ": "
                     << first.allocations_ - second.allocations_ << " allocations, "
                     << first.bytes_ - second.bytes_ << " bytes";
    EXPECT_GE(first.allocations_ - second.allocations_, 2 * NUM_DATAGRAMS_PER_RECEIVE);
    EXPECT_LE(first.bytes_ - second.bytes_, bound);
    return;
  }
  if (Memory::Stats::totalCurrentlyAllocated() == 0) {
    GTEST_SKIP() << "the memory in use is only known in an ASAN build or with tcmalloc";
  }
  // Without allocation hooks, what is measured is the memory in use after the first read of an
  // empty socket against before it: everything else that the read allocates is freed by then.
  const uint64_t before = Memory::Stats::totalCurrentlyAllocated();
  expectAgain(readEvent());
  const uint64_t after = Memory::Stats::totalCurrentlyAllocated();
  EXPECT_EQ(NUM_DATAGRAMS_PER_RECEIVE, processor_.keptBuffersAllocated());
  ASSERT_GE(after, before);
  GTEST_LOG_(INFO) << "kept receive buffers for a datagram size of "
                   << processor_.max_datagram_size_ << ": " << after - before
                   << " bytes in use (tcmalloc)";
  EXPECT_GE(after - before, NUM_DATAGRAMS_PER_RECEIVE * processor_.max_datagram_size_);
  EXPECT_LE(after - before, bound);
}

// Heap allocations of one read event that finds one datagram. Only counted in an ASAN build.
TEST_P(UdpRecvMmsgTest, AllocationsForOneDatagramPerEvent) {
  if (!AllocationCounter::available()) {
    GTEST_SKIP() << "heap allocations are only counted in an ASAN build";
  }
  // The first events fill caches and create objects that live for the process.
  for (uint32_t i = 0; i < 3; i++) {
    sys_calls_.expect({{QuicDatagramSize, 'a'}});
    expectAgain(readEvent());
  }
  constexpr uint32_t events = 10;
  for (uint32_t i = 0; i < events; i++) {
    sys_calls_.expect({{QuicDatagramSize, 'a'}});
  }
  const size_t calls_before = recvmmsgCalls();
  const uint64_t kept_before = processor_.keptBuffersAllocated();
  AllocationCounter::start();
  for (uint32_t i = 0; i < events; i++) {
    const Api::IoErrorPtr error = readEvent();
    if (error == nullptr || error->getErrorCode() != Api::IoError::IoErrorCode::Again) {
      AllocationCounter::stop();
      FAIL() << "unexpected result of a read event";
    }
  }
  const AllocationCounter::Counts counts = AllocationCounter::stop();
  const double calls = static_cast<double>(recvmmsgCalls() - calls_before) / events;
  const double allocations = static_cast<double>(counts.allocations_) / events;
  const double bytes = static_cast<double>(counts.bytes_) / events;
  GTEST_LOG_(INFO) << "one datagram per read event, kept receive buffers "
                   << (reuse() ? "on" : "off") << ": recvmmsg calls " << calls
                   << ", heap allocations " << allocations << ", bytes allocated " << bytes
                   << ", receive buffers allocated to be kept "
                   << static_cast<double>(processor_.keptBuffersAllocated() - kept_before) / events;
  RecordProperty("recvmmsg_calls_per_event", std::to_string(calls));
  RecordProperty("allocations_per_event", std::to_string(allocations));
  EXPECT_EQ(2, calls);
  if (reuse()) {
    // One buffer object and its memory for the datagram, and what a call allocates besides its
    // receive buffers.
    EXPECT_LT(allocations, 2 * NUM_DATAGRAMS_PER_RECEIVE);
  } else {
    // Every call builds a full set of receive buffers: a buffer object and its memory for each.
    EXPECT_GE(allocations, 2 * NUM_DATAGRAMS_PER_RECEIVE * calls);
  }
}

} // namespace
} // namespace Network
} // namespace Envoy

#endif // defined(__linux__)
