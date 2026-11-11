// Tests for the nghttp2 HPACK deflater self-eviction guard carried in
// bazel/external/nghttp2_hpack_eviction_guard.patch.
//
// When a header block is larger than the HPACK dynamic table, the unpatched deflater inserts every
// field and evicts the fields the previous identical block inserted, so no field is ever found in
// the table. With the guard, the part of the block that fits stays resident and is sent as 1 byte
// index representations on every later block.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "nghttp2/nghttp2.h"

namespace Envoy {
namespace Http {
namespace Http2 {
namespace {

// HPACK entry overhead (RFC 7541 section 4.1).
constexpr size_t kEntryOverhead = 32;
constexpr size_t kNameLen = 10;
constexpr size_t kValueLen = 62;
// Table room of one header field produced by makeHeaders().
constexpr size_t kEntryRoom = kEntryOverhead + kNameLen + kValueLen;
constexpr size_t kTableSize = 4096;
// Number of such fields a 4096 byte table holds.
constexpr size_t kFit = kTableSize / kEntryRoom;
constexpr size_t kStaticTableLength = 61;

using HeaderList = std::vector<std::pair<std::string, std::string>>;

// Makes `n` distinct header fields "x-<set>-hdr-NN" of kEntryRoom table bytes each.
HeaderList makeHeaders(char set, size_t n) {
  HeaderList headers;
  for (size_t i = 0; i < n; ++i) {
    std::string name = std::string("x-") + set + "-hdr-" + static_cast<char>('0' + i / 10) +
                       static_cast<char>('0' + i % 10);
    EXPECT_EQ(kNameLen, name.size());
    headers.emplace_back(std::move(name), std::string(kValueLen, static_cast<char>('a' + i % 26)));
  }
  return headers;
}

class HpackEvictionGuardTest : public testing::Test {
protected:
  void SetUp() override {
    ASSERT_EQ(0, nghttp2_hd_deflate_new(&deflater_, kTableSize));
    ASSERT_EQ(0, nghttp2_hd_inflate_new(&inflater_));
  }

  void TearDown() override {
    nghttp2_hd_deflate_del(deflater_);
    nghttp2_hd_inflate_del(inflater_);
  }

  // Encodes `headers` as one header block, checks that the inflater decodes it back exactly, and
  // returns the size of the encoded block.
  size_t roundTrip(const HeaderList& headers) {
    std::vector<nghttp2_nv> nva;
    nva.reserve(headers.size());
    for (const auto& [name, value] : headers) {
      nva.push_back({const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(name.data())),
                     const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(value.data())),
                     name.size(), value.size(), NGHTTP2_NV_FLAG_NONE});
    }

    std::vector<uint8_t> buf(nghttp2_hd_deflate_bound(deflater_, nva.data(), nva.size()));
    const nghttp2_ssize encoded =
        nghttp2_hd_deflate_hd2(deflater_, buf.data(), buf.size(), nva.data(), nva.size());
    EXPECT_GT(encoded, 0);
    if (encoded <= 0) {
      return 0;
    }

    HeaderList decoded;
    const uint8_t* in = buf.data();
    size_t inlen = static_cast<size_t>(encoded);
    for (;;) {
      nghttp2_nv nv;
      int inflate_flags = 0;
      const nghttp2_ssize consumed =
          nghttp2_hd_inflate_hd3(inflater_, &nv, &inflate_flags, in, inlen, /*in_final=*/1);
      EXPECT_GE(consumed, 0);
      if (consumed < 0) {
        return 0;
      }
      in += consumed;
      inlen -= static_cast<size_t>(consumed);
      if (inflate_flags & NGHTTP2_HD_INFLATE_EMIT) {
        decoded.emplace_back(std::string(reinterpret_cast<const char*>(nv.name), nv.namelen),
                             std::string(reinterpret_cast<const char*>(nv.value), nv.valuelen));
      }
      if (inflate_flags & NGHTTP2_HD_INFLATE_FINAL) {
        nghttp2_hd_inflate_end_headers(inflater_);
        break;
      }
      if (inlen == 0 && !(inflate_flags & NGHTTP2_HD_INFLATE_EMIT)) {
        ADD_FAILURE() << "inflater made no progress";
        return 0;
      }
    }
    EXPECT_EQ(headers, decoded);
    return static_cast<size_t>(encoded);
  }

  size_t dynamicEntries() {
    return nghttp2_hd_deflate_get_num_table_entries(deflater_) - kStaticTableLength;
  }

  nghttp2_hd_deflater* deflater_{};
  nghttp2_hd_inflater* inflater_{};
};

// A repeated header set larger than the table: later blocks must be much smaller than the first.
TEST_F(HpackEvictionGuardTest, RepeatedOversizedHeaderSetStaysPartlyIndexed) {
  ASSERT_EQ(39U, kFit);
  const HeaderList headers = makeHeaders('a', 52);

  const size_t first = roundTrip(headers);
  EXPECT_EQ(kFit, dynamicEntries());

  // The newest dynamic entry is the last field that fit, not the last field of the block.
  const nghttp2_nv* newest =
      nghttp2_hd_deflate_get_table_entry(deflater_, kStaticTableLength + 1);
  ASSERT_NE(nullptr, newest);
  EXPECT_EQ(headers[kFit - 1].first,
            std::string(reinterpret_cast<const char*>(newest->name), newest->namelen));

  const size_t second = roundTrip(headers);
  const size_t third = roundTrip(headers);
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(third, roundTrip(headers));
  }
  EXPECT_EQ(second, third);
  // 39 fields are 1 byte index representations; only 13 are literals. Unpatched nghttp2 sends
  // all 52 as literals every time, so `second` would be about the size of `first`.
  EXPECT_LT(second * 3, first);
  EXPECT_EQ(kFit, dynamicEntries());
}

// A header set that fits the table is unaffected: fully indexed from the second block on.
TEST_F(HpackEvictionGuardTest, FittingHeaderSetFullyIndexed) {
  const HeaderList headers = makeHeaders('a', 30);
  roundTrip(headers);
  EXPECT_EQ(30U, dynamicEntries());
  EXPECT_EQ(30U, roundTrip(headers));
  EXPECT_EQ(30U, roundTrip(headers));
}

// Two disjoint header sets alternating on one connection, each fitting alone but not together:
// one set stays resident instead of both thrashing.
TEST_F(HpackEvictionGuardTest, AlternatingHeaderSetsKeepOneResident) {
  const HeaderList a = makeHeaders('c', 30);
  const HeaderList b = makeHeaders('d', 30);

  roundTrip(a);
  const size_t b_first = roundTrip(b);
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(30U, roundTrip(a));
    EXPECT_LT(roundTrip(b), b_first);
  }
  EXPECT_EQ(kFit, dynamicEntries());
}

// A permanent change of header set is adopted after one block.
TEST_F(HpackEvictionGuardTest, NewHeaderSetReplacesOldOne) {
  const HeaderList old_headers = makeHeaders('a', 52);
  const HeaderList new_headers = makeHeaders('b', 52);
  roundTrip(old_headers);
  roundTrip(old_headers);

  const size_t first = roundTrip(new_headers);
  roundTrip(new_headers);
  const size_t steady = roundTrip(new_headers);
  EXPECT_EQ(steady, roundTrip(new_headers));
  EXPECT_LT(steady * 3, first);
}

} // namespace
} // namespace Http2
} // namespace Http
} // namespace Envoy
