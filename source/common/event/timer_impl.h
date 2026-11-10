#pragma once

#include <chrono>

#include "envoy/event/timer.h"

#include "source/common/common/scope_tracker.h"
#include "source/common/common/utility.h"
#include "source/common/event/event_impl_base.h"
#include "source/common/event/libevent.h"

namespace Envoy {
namespace Event {

/**
 * Utility helper functions for Timer implementation.
 */
class TimerUtils {
public:
  /**
   * Intended for consumption by enable(HR)Timer, this method is templated method to avoid implicit
   * duration conversions for its input arguments. This lets us have an opportunity to check bounds
   * before doing any conversions. When the passed in duration exceeds INT32_MAX max seconds, the
   * output will be clipped to yield INT32_MAX seconds and 0 microseconds for the
   * output argument. We clip to INT32_MAX to guard against overflowing the timeval structure.
   * ENVOY_BUGs if the duration is negative.
   * @tparam Duration std::chrono duration type, e.g. seconds, milliseconds, ...
   * @param d duration value
   * @param tv output parameter that will be updated
   */
  template <typename Duration> static void durationToTimeval(const Duration& d, timeval& tv) {
    if (d.count() < 0) {
      IS_ENVOY_BUG(fmt::format("Negative duration passed to durationToTimeval(): {}", d.count()));
      tv.tv_sec = 0;
      tv.tv_usec = 500000;
      return;
    };
    constexpr int64_t clip_to = INT32_MAX; // 136.102208 years
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(d);
    if (secs.count() > clip_to) {
      tv.tv_sec = clip_to;
      tv.tv_usec = 0;
      return;
    }

    auto usecs = std::chrono::duration_cast<std::chrono::microseconds>(d - secs);
    tv.tv_sec = secs.count();
    tv.tv_usec = usecs.count();
  }
};

/**
 * libevent implementation of Timer.
 */
class TimerImpl : public Timer, ImplBase {
public:
  TimerImpl(Libevent::BasePtr& libevent, TimerCb cb, Event::Dispatcher& dispatcher);

  // Timer
  void disableTimer() override;

  void enableTimer(std::chrono::milliseconds d, const ScopeTrackedObject* scope) override;
  void enableHRTimer(std::chrono::microseconds us, const ScopeTrackedObject* object) override;

  bool enabled() override;

  // Per-request timer objects (stream idle/request timeouts, per-try/response timeouts) are
  // created, armed, canceled and freed in tight per-request loops on the dispatcher thread.
  // Recycle their storage through a fixed-capacity thread-local free list instead of hitting the
  // allocator on every createTimer/destroy pair; the embedded libevent event is fully
  // re-initialized by the constructor (evtimer_assign), so a recycled object is fresh by
  // construction. Dispatchers are single-threaded, and a TimerImpl lives and dies on its owning
  // dispatcher's thread, so a thread-local free list is sound. Timer's destructor is virtual, so
  // deleting through a TimerPtr selects these most-derived operators (including for subclasses).
  static void* operator new(size_t size);
  static void operator delete(void* ptr);

private:
  void internalEnableTimer(const timeval& tv, const ScopeTrackedObject* scope);
  TimerCb cb_;
  Dispatcher& dispatcher_;
  // This has to be atomic for alarms which are handled out of thread, for
  // example if the DispatcherImpl::post is called by two threads, they race to
  // both set this to null.
  std::atomic<const ScopeTrackedObject*> object_{nullptr};
};

} // namespace Event
} // namespace Envoy
