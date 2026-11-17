#include "source/common/stats/main_thread_utility.h"

#include "source/common/common/thread.h"

namespace Envoy {
namespace Stats {
namespace Utility {

std::vector<Counter*> countersMainThread(Store& store) {
  ASSERT_IS_MAIN_OR_TEST_THREAD();
  std::vector<Counter*> counters;
  store.forEachCounter([&counters](size_t size) { counters.reserve(size); },
                       [&counters](Counter& counter) { counters.push_back(&counter); });
  return counters;
}

std::vector<Gauge*> gaugesMainThread(Store& store) {
  ASSERT_IS_MAIN_OR_TEST_THREAD();
  std::vector<Gauge*> gauges;
  store.forEachGauge([&gauges](size_t size) { gauges.reserve(size); },
                     [&gauges](Gauge& gauge) { gauges.push_back(&gauge); });
  return gauges;
}

std::vector<TextReadout*> textReadoutsMainThread(Store& store) {
  ASSERT_IS_MAIN_OR_TEST_THREAD();
  std::vector<TextReadout*> text_readouts;
  store.forEachTextReadout(
      [&text_readouts](size_t size) { text_readouts.reserve(size); },
      [&text_readouts](TextReadout& text_readout) { text_readouts.push_back(&text_readout); });
  return text_readouts;
}

std::vector<ParentHistogram*> histogramsMainThread(Store& store) {
  ASSERT_IS_MAIN_OR_TEST_THREAD();
  std::vector<ParentHistogram*> histograms;
  store.forEachHistogram(
      [&histograms](size_t size) { histograms.reserve(size); },
      [&histograms](ParentHistogram& histogram) { histograms.push_back(&histogram); });
  return histograms;
}

} // namespace Utility
} // namespace Stats
} // namespace Envoy
