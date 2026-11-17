#pragma once

#include "source/common/stats/allocator.h"

namespace Envoy {
namespace Stats {
namespace TestUtil {

/**
 * Lets tests create stats directly in an Allocator. Production code must create stats through a
 * Scope, which owns them; see the comment on Allocator::makeCounter().
 */
class AllocatorPeer {
public:
  static CounterSharedPtr makeCounter(Allocator& alloc, StatName name, StatName tag_extracted_name,
                                      StatNameTagSpan stat_name_tags) {
    return alloc.makeCounter(name, tag_extracted_name, stat_name_tags);
  }

  static GaugeSharedPtr makeGauge(Allocator& alloc, StatName name, StatName tag_extracted_name,
                                  StatNameTagSpan stat_name_tags, Gauge::ImportMode import_mode) {
    return alloc.makeGauge(name, tag_extracted_name, stat_name_tags, import_mode);
  }

  static TextReadoutSharedPtr makeTextReadout(Allocator& alloc, StatName name,
                                              StatName tag_extracted_name,
                                              StatNameTagSpan stat_name_tags) {
    return alloc.makeTextReadout(name, tag_extracted_name, stat_name_tags);
  }
};

} // namespace TestUtil
} // namespace Stats
} // namespace Envoy
