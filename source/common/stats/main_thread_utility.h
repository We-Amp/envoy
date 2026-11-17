#pragma once

#include <vector>

#include "envoy/stats/histogram.h"
#include "envoy/stats/stats.h"
#include "envoy/stats/store.h"

namespace Envoy {
namespace Stats {
namespace Utility {

/**
 * Returns vectors of stats held in the store. This must only be called from the
 * main thread. The stats are returned as pointers, and the pointers are
 * guaranteed to remain valid only until the main thread is released -- this is
 * because stats are always deleted on the main thread.
 *
 * If the last reference to a Stats::ScopeSharedPtr is removed during or after
 * the call to construct these vectors, the stats will still remain valid until
 * the current call-sequence on the main thread completes.
 *
 * These functions handle de-duplication.
 *
 * These live in their own library, rather than in utility.h, because they need
 * the Store interface, which utility_lib cannot depend on without a dependency
 * cycle through stats_macros.
 */
std::vector<Counter*> countersMainThread(Store& store);
std::vector<Gauge*> gaugesMainThread(Store& store);
std::vector<TextReadout*> textReadoutsMainThread(Store& store);
std::vector<ParentHistogram*> histogramsMainThread(Store& store);

} // namespace Utility
} // namespace Stats
} // namespace Envoy
