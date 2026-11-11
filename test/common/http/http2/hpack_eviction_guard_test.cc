// Tests for the nghttp2 HPACK deflater self-eviction guard carried in
// bazel/external/nghttp2_hpack_eviction_guard.patch.
//
// When a header block is larger than the HPACK dynamic table, the unpatched deflater inserts every
// field and evicts the fields the previous identical block inserted, so no field is ever found in
// the table. With the guard, the part of the block that fits stays resident and is sent as 1 byte
// index representations on every later block. The guard is enabled only for a header block whose
// own indexable fields need more room than the table has; every other block is encoded exactly as
// by unpatched nghttp2.

#include <cstdint>
#include <deque>
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

// A date-like field whose value changes per block. Table room: 32 + 4 + 29 = 65 bytes.
std::pair<std::string, std::string> dateField(size_t n) {
  std::string value = "Wed, 11 Nov 2026 00:00:00 GMT";
  value[17] = static_cast<char>('0' + (n / 1000) % 10);
  value[18] = static_cast<char>('0' + (n / 100) % 10);
  value[20] = static_cast<char>('0' + (n / 10) % 10);
  value[21] = static_cast<char>('0' + n % 10);
  return {"date", value};
}

// Model of the dynamic table of the UNPATCHED nghttp2 deflater: every field that is not an exact
// match and is not larger than 3/4 of the table is inserted at the front, and the oldest entries
// are evicted until the table fits. Only valid for the header names used in these tests (none of
// them is in nghttp2's never-index list, and no field is an exact static table match).
class UnpatchedTableModel {
public:
  explicit UnpatchedTableModel(size_t max) : max_(max) {}

  void encode(const HeaderList& headers) {
    for (const auto& field : headers) {
      bool found = false;
      for (const auto& entry : entries_) {
        if (entry == field) {
          found = true;
          break;
        }
      }
      const size_t room = kEntryOverhead + field.first.size() + field.second.size();
      if (found || room > max_ * 3 / 4) {
        continue;
      }
      while (size_ + room > max_ && !entries_.empty()) {
        const auto& oldest = entries_.back();
        size_ -= kEntryOverhead + oldest.first.size() + oldest.second.size();
        entries_.pop_back();
      }
      entries_.push_front(field);
      size_ += room;
    }
  }

  HeaderList entries() const { return {entries_.begin(), entries_.end()}; }

private:
  const size_t max_;
  size_t size_{0};
  std::deque<std::pair<std::string, std::string>> entries_;
};

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

  // The deflater's dynamic table, newest entry first.
  HeaderList dynamicTable() {
    HeaderList table;
    const size_t n = dynamicEntries();
    for (size_t i = 0; i < n; ++i) {
      const nghttp2_nv* nv =
          nghttp2_hd_deflate_get_table_entry(deflater_, kStaticTableLength + 1 + i);
      if (nv == nullptr) {
        ADD_FAILURE() << "missing dynamic table entry " << i;
        break;
      }
      table.emplace_back(std::string(reinterpret_cast<const char*>(nv->name), nv->namelen),
                         std::string(reinterpret_cast<const char*>(nv->value), nv->valuelen));
    }
    return table;
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

// Two disjoint header sets alternating on one connection, each larger than the table: the leading
// part of the first set stays resident and keeps hitting instead of both sets thrashing.
TEST_F(HpackEvictionGuardTest, AlternatingOversizedHeaderSetsKeepOneResident) {
  const HeaderList a = makeHeaders('c', 45);
  const HeaderList b = makeHeaders('d', 45);

  const size_t a_first = roundTrip(a);
  const size_t b_first = roundTrip(b);
  const size_t a_steady = roundTrip(a);
  // 39 one-byte indexes plus 6 literals.
  EXPECT_LT(a_steady * 3, a_first);
  for (int i = 0; i < 4; ++i) {
    // b would evict entries a used in the previous block, so it is sent without indexing.
    EXPECT_EQ(b_first, roundTrip(b));
    EXPECT_EQ(a_steady, roundTrip(a));
  }
  EXPECT_EQ(kFit, dynamicEntries());
}

// Header blocks that fit the table are encoded exactly as by unpatched nghttp2, also on a
// long-lived connection where the table is full and churning: after every block the deflater's
// dynamic table equals the unpatched FIFO model. Identical tables after every block mean every
// field got the same representation, so the output is byte-for-byte the unpatched output.
TEST_F(HpackEvictionGuardTest, FittingBlocksMatchUnpatchedTable) {
  UnpatchedTableModel model(kTableSize);
  const HeaderList stable = makeHeaders('s', 20);
  const HeaderList a = makeHeaders('c', 30);
  const HeaderList b = makeHeaders('d', 30);

  for (size_t n = 0; n < 150; ++n) {
    HeaderList block;
    switch (n % 3) {
    case 0:
      block = stable;
      block.push_back(dateField(n));
      break;
    case 1:
      // Fits alone, but not together with the other blocks: plain FIFO churn.
      block = a;
      break;
    default:
      block = b;
      block.push_back(dateField(n));
      break;
    }
    roundTrip(block);
    model.encode(block);
    ASSERT_EQ(model.entries(), dynamicTable()) << "block " << n;
  }
}

// A stable header set that fits, plus a date-like field that changes on every block, over more
// than two table fills. The table must not freeze: a new value is still indexed once the table is
// full, so a value that repeats is sent as a 1 byte index.
TEST_F(HpackEvictionGuardTest, ChangingFieldDoesNotFreezeTable) {
  UnpatchedTableModel model(kTableSize);
  const HeaderList stable = makeHeaders('s', 20);

  // 20 * 104 = 2080 bytes stable; each date is 65 bytes, so about 31 dates fill the rest. 120
  // blocks is close to four fills of that remainder.
  for (size_t n = 0; n < 120; ++n) {
    HeaderList block = stable;
    block.push_back(dateField(n));
    roundTrip(block);
    model.encode(block);
    ASSERT_EQ(model.entries(), dynamicTable()) << "block " << n;
  }

  // A new value after many fills is still inserted: it is the newest entry.
  HeaderList block = stable;
  block.push_back(dateField(9999));
  roundTrip(block);
  const HeaderList table = dynamicTable();
  ASSERT_FALSE(table.empty());
  EXPECT_EQ(dateField(9999), table.front());

  // Sent again, the repeated value is a 1 byte index. A value never sent before is a literal.
  EXPECT_EQ(1U, roundTrip({dateField(9999)}));
  EXPECT_GT(roundTrip({dateField(8888)}), 10U);
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
