#include "source/common/event/file_event_impl.h"

#include <cstdint>

#include "source/common/common/assert.h"
#include "source/common/event/dispatcher_impl.h"
#include "source/common/runtime/runtime_features.h"

#include "event2/event.h"

namespace Envoy {
namespace Event {

FileEventImpl::FileEventImpl(DispatcherImpl& dispatcher, os_fd_t fd, FileReadyCb cb,
                             FileTriggerType trigger, uint32_t events)
    : dispatcher_(dispatcher), cb_(cb), fd_(fd), trigger_(trigger), enabled_events_(events),
      activation_cb_(dispatcher.createSchedulableCallback([this]() {
        ASSERT(injected_activation_events_ != 0);
        mergeInjectedEventsAndRunCb(0);
      })),
      write_in_current_iteration_(
          trigger == FileTriggerType::Edge &&
          Runtime::runtimeFeatureEnabled(
              "envoy.reloadable_features.file_event_write_in_current_iteration")) {
  // Treat the lack of a valid fd (which in practice should only happen if we run out of FDs) as
  // an OOM condition and just crash.
  RELEASE_ASSERT(SOCKET_VALID(fd), "");
#ifdef WIN32
  ASSERT(trigger_ != FileTriggerType::Edge, "libevent does not support edge triggers on Windows");
#endif
  if constexpr (PlatformDefaultTriggerType != FileTriggerType::EmulatedEdge) {
    ASSERT(trigger_ != FileTriggerType::EmulatedEdge,
           "Cannot use EmulatedEdge events if they are not the default platform type");
  }

  assignEvents(events, &dispatcher.base());
  event_add(&raw_event_, nullptr);
}

void FileEventImpl::activate(uint32_t events) {
  ASSERT(dispatcher_.isThreadSafe());

  // events is not empty.
  ASSERT(events != 0);
  // Only supported event types are set.
  ASSERT((events & (FileReadyType::Read | FileReadyType::Write | FileReadyType::Closed)) == events);

  if (maybeScheduleWriteInCurrentIteration(events)) {
    return;
  }

  // Schedule the activation callback so it runs as part of the next loop iteration if it is not
  // already scheduled.
  if (injected_activation_events_ == 0) {
    ASSERT(!activation_cb_->enabled());
    activation_cb_->scheduleCallbackNextIteration();
  }
  ASSERT(activation_cb_->enabled());

  // Merge new events with pending injected events.
  injected_activation_events_ |= events;
}

bool FileEventImpl::maybeScheduleWriteInCurrentIteration(uint32_t events) {
  // Only an activation of exactly Write is delivered in the current iteration. Read and Closed
  // activations, alone or together with Write, are delivered in the next iteration as before, and
  // are never merged into the delivery scheduled here.
  if (!write_in_current_iteration_ || events != FileReadyType::Write) {
    return false;
  }
  if (current_iteration_write_pending_) {
    // The Write event is already on its way in this iteration.
    return true;
  }
  // The activation is delivered in the next iteration as before if
  // - an injected event is already pending: the next iteration will not wait for fd events anyway,
  //   and the pending delivery picks up this Write event, or
  // - an injected Write event was already scheduled for, or delivered in, the current iteration:
  //   at most one injected Write event per file event per iteration bounds the work that
  //   activations made from the Write callback itself can add to an iteration, and keeps the event
  //   loop polling for fd events.
  if (injected_activation_events_ != 0 ||
      current_iteration_write_iteration_ == dispatcher_.event_loop_iteration_) {
    return false;
  }
  if (current_iteration_write_cb_ == nullptr) {
    current_iteration_write_cb_ =
        dispatcher_.createSchedulableCallback([this]() { runWriteInCurrentIteration(); });
  }
  current_iteration_write_iteration_ = dispatcher_.event_loop_iteration_;
  current_iteration_write_pending_ = true;
  // The callback is appended to the work list of the current iteration. It runs after the events
  // that are already on the list and never inside the caller of activate().
  current_iteration_write_cb_->scheduleCallbackCurrentIteration();
  return true;
}

void FileEventImpl::runWriteInCurrentIteration() {
  ASSERT(dispatcher_.isThreadSafe());
  ASSERT(current_iteration_write_pending_);
  current_iteration_write_pending_ = false;
  // The event may have been scheduled before the event loop ran, which is before the iteration that
  // delivers it.
  current_iteration_write_iteration_ = dispatcher_.event_loop_iteration_;
  // The callback may destroy this file event.
  THROW_IF_NOT_OK(cb_(FileReadyType::Write));
}

void FileEventImpl::assignEvents(uint32_t events, event_base* base) {
  ASSERT(dispatcher_.isThreadSafe());
  ASSERT(base != nullptr);

  enabled_events_ = events;
  event_assign(
      &raw_event_, base, fd_,
      EV_PERSIST | (trigger_ == FileTriggerType::Edge ? EV_ET : 0) |
          (events & FileReadyType::Read ? EV_READ : 0) |
          (events & FileReadyType::Write ? EV_WRITE : 0) |
          (events & FileReadyType::Closed ? EV_CLOSED : 0),
      [](evutil_socket_t, short what, void* arg) -> void {
        auto* event = static_cast<FileEventImpl*>(arg);
        uint32_t events = 0;
        if (what & EV_READ) {
          events |= FileReadyType::Read;
        }

        if (what & EV_WRITE) {
          events |= FileReadyType::Write;
        }

        if (what & EV_CLOSED) {
          events |= FileReadyType::Closed;
        }

        ASSERT(events != 0);
        event->mergeInjectedEventsAndRunCb(events);
      },
      this);
}

void FileEventImpl::updateEvents(uint32_t events) {
  ASSERT(dispatcher_.isThreadSafe());
  // The update can be skipped in cases where the old and new event mask are the same if the fd is
  // using Level or EmulatedEdge trigger modes, but not Edge trigger mode. When the fd is registered
  // in edge trigger mode, re-registering the fd will force re-computation of the readable/writable
  // state even in cases where the event mask is not changing. See
  // https://github.com/envoyproxy/envoy/pull/16389 for more details.
  // TODO(antoniovicente) Consider ways to optimize away event registration updates in edge trigger
  // mode once setEnabled stops clearing injected_activation_events_ before calling updateEvents
  // and/or implement optimizations at the Network::ConnectionImpl level to reduce the number of
  // calls to setEnabled.
  if (events == enabled_events_ && trigger_ != FileTriggerType::Edge) {
    return;
  }
  auto* base = event_get_base(&raw_event_);
  event_del(&raw_event_);
  assignEvents(events, base);
  event_add(&raw_event_, nullptr);
}

void FileEventImpl::setEnabled(uint32_t events) {
  ASSERT(dispatcher_.isThreadSafe());
  if (injected_activation_events_ != 0) {
    // Clear pending events on updates to the fd event mask to avoid delivering events that are no
    // longer relevant. Updating the event mask will reset the fd edge trigger state so the proxy
    // will be able to determine the fd read/write state without need for the injected activation
    // events.
    injected_activation_events_ = 0;
    activation_cb_->cancel();
  }
  if (current_iteration_write_pending_) {
    // Same for a Write event that is scheduled for the current iteration.
    current_iteration_write_pending_ = false;
    current_iteration_write_cb_->cancel();
  }
  updateEvents(events);
}

void FileEventImpl::unregisterEventIfEmulatedEdge(uint32_t event) {
  ASSERT(dispatcher_.isThreadSafe());
  // This constexpr if allows the compiler to optimize away the function on POSIX
  if constexpr (PlatformDefaultTriggerType == FileTriggerType::EmulatedEdge) {
    if (trigger_ == FileTriggerType::EmulatedEdge) {
      auto new_event_mask = enabled_events_ & ~event;
      updateEvents(new_event_mask);
    }
  }
}

void FileEventImpl::registerEventIfEmulatedEdge(uint32_t event) {
  ASSERT(dispatcher_.isThreadSafe());
  // This constexpr if allows the compiler to optimize away the function on POSIX
  if constexpr (PlatformDefaultTriggerType == FileTriggerType::EmulatedEdge) {
    ASSERT((event & (FileReadyType::Read | FileReadyType::Write)) == event);
    if (trigger_ == FileTriggerType::EmulatedEdge) {
      auto new_event_mask = enabled_events_ | event;
      updateEvents(new_event_mask);
    }
  }
}

void FileEventImpl::mergeInjectedEventsAndRunCb(uint32_t events) {
  ASSERT(dispatcher_.isThreadSafe());
  if (injected_activation_events_ != 0) {
    if (write_in_current_iteration_ && (injected_activation_events_ & FileReadyType::Write)) {
      // An injected Write event is delivered in this iteration, so a Write activation made later
      // in this iteration is delivered in the next one, see maybeScheduleWriteInCurrentIteration().
      current_iteration_write_iteration_ = dispatcher_.event_loop_iteration_;
    }
    events |= injected_activation_events_;
    injected_activation_events_ = 0;
    activation_cb_->cancel();
  }
  if (current_iteration_write_pending_) {
    // This callback also delivers the Write event that is scheduled for the current iteration.
    events |= FileReadyType::Write;
    current_iteration_write_pending_ = false;
    current_iteration_write_iteration_ = dispatcher_.event_loop_iteration_;
    current_iteration_write_cb_->cancel();
  }

  // TODO(davinci26): This can be optimized further in (w)epoll backends using the `EPOLLONESHOT`
  // flag. With this flag `EPOLLIN`/`EPOLLOUT` are automatically disabled when the event is
  // activated.
  if constexpr (PlatformDefaultTriggerType == FileTriggerType::EmulatedEdge) {
    if (trigger_ == FileTriggerType::EmulatedEdge) {
      unregisterEventIfEmulatedEdge(events &
                                    (Event::FileReadyType::Write | Event::FileReadyType::Read));
    }
  }

  THROW_IF_NOT_OK(cb_(events));
}

} // namespace Event
} // namespace Envoy
