#include "source/common/event/timer_impl.h"

#include <chrono>
#include <vector>

#include "source/common/common/assert.h"

#include "event2/event.h"

namespace Envoy {
namespace Event {

TimerImpl::TimerImpl(Libevent::BasePtr& libevent, TimerCb cb, Dispatcher& dispatcher)
    : cb_(cb), dispatcher_(dispatcher) {
  ASSERT(cb_);
  evtimer_assign(
      &raw_event_, libevent.get(),
      [](evutil_socket_t, short, void* arg) -> void {
        TimerImpl* timer = static_cast<TimerImpl*>(arg);
        if (timer->object_ == nullptr) {
          timer->cb_();
          return;
        }
        ScopeTrackerScopeState scope(timer->object_, timer->dispatcher_);
        timer->object_ = nullptr;
        timer->cb_();
      },
      this);
}

void TimerImpl::disableTimer() {
  ASSERT(dispatcher_.isThreadSafe());
  event_del(&raw_event_);
}

void TimerImpl::enableTimer(const std::chrono::milliseconds d, const ScopeTrackedObject* object) {
  timeval tv;
  TimerUtils::durationToTimeval(d, tv);
  internalEnableTimer(tv, object);
}

void TimerImpl::enableHRTimer(const std::chrono::microseconds d,
                              const ScopeTrackedObject* object = nullptr) {
  timeval tv;
  TimerUtils::durationToTimeval(d, tv);
  internalEnableTimer(tv, object);
}

void TimerImpl::internalEnableTimer(const timeval& tv, const ScopeTrackedObject* object) {
  ASSERT(dispatcher_.isThreadSafe());
  object_ = object;

  event_add(&raw_event_, &tv);
}

bool TimerImpl::enabled() {
  ASSERT(dispatcher_.isThreadSafe());
  return 0 != evtimer_pending(&raw_event_, nullptr);
}

namespace {
// Fixed-capacity per-thread free list of TimerImpl storage. See the operator new/delete comment
// in timer_impl.h.
constexpr size_t kTimerFreeListCapacity = 64;

struct TimerFreeList {
  ~TimerFreeList() {
    for (void* ptr : slots) {
      ::operator delete(ptr);
    }
  }
  std::vector<void*> slots;
};

thread_local TimerFreeList timer_free_list;
} // namespace

void* TimerImpl::operator new(size_t size) {
  if (size != sizeof(TimerImpl)) {
    // A subclass with extra members; the pooled blocks are the wrong size.
    return ::operator new(size);
  }
  if (!timer_free_list.slots.empty()) {
    void* ptr = timer_free_list.slots.back();
    timer_free_list.slots.pop_back();
    return ptr;
  }
  return ::operator new(size);
}

void TimerImpl::operator delete(void* ptr) {
  if (timer_free_list.slots.size() < kTimerFreeListCapacity) {
    // ~ImplBase has already event_del()ed the embedded event, so a parked timer cannot fire and
    // cb_ is re-bound by the next construction before any arm.
    timer_free_list.slots.push_back(ptr);
    return;
  }
  ::operator delete(ptr);
}

} // namespace Event
} // namespace Envoy
