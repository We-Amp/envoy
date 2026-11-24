#pragma once

#include <cstdint>

#include "envoy/event/file_event.h"

#include "source/common/event/dispatcher_impl.h"
#include "source/common/event/event_impl_base.h"

namespace Envoy {
namespace Event {

/**
 * Implementation of FileEvent for libevent that uses persistent events and
 * assumes the user will read/write until EAGAIN is returned from the file.
 */
class FileEventImpl : public FileEvent, ImplBase {
public:
  FileEventImpl(DispatcherImpl& dispatcher, os_fd_t fd, FileReadyCb cb, FileTriggerType trigger,
                uint32_t events);

  // Event::FileEvent
  void activate(uint32_t events) override;
  void setEnabled(uint32_t events) override;
  void unregisterEventIfEmulatedEdge(uint32_t event) override;
  void registerEventIfEmulatedEdge(uint32_t event) override;

private:
  void assignEvents(uint32_t events, event_base* base);
  void mergeInjectedEventsAndRunCb(uint32_t events);
  void updateEvents(uint32_t events);
  bool maybeScheduleWriteInCurrentIteration(uint32_t events);
  void runWriteInCurrentIteration();

  DispatcherImpl& dispatcher_;
  FileReadyCb cb_;
  os_fd_t fd_;
  FileTriggerType trigger_;
  // Enabled events for this fd.
  uint32_t enabled_events_;

  // Injected FileReadyType events that were scheduled by recent calls to activate() and are pending
  // delivery.
  uint32_t injected_activation_events_{};
  // Used to schedule delayed event activation. Armed iff pending_activation_events_ != 0.
  SchedulableCallbackPtr activation_cb_;

  // True if activate(FileReadyType::Write) may deliver the event in the current event loop
  // iteration: the file event is edge-triggered and the runtime guard
  // envoy.reloadable_features.connection_write_in_current_iteration was enabled when the file
  // event was created.
  const bool write_in_current_iteration_;
  // True while current_iteration_write_cb_ is scheduled and has not run.
  bool current_iteration_write_pending_{false};
  // The last event loop iteration in which an injected Write event was scheduled on
  // current_iteration_write_cb_ or delivered by activation_cb_; 0 if none. A Write activation is
  // only scheduled for the current iteration if this is not the current iteration, so activations
  // that trigger each other cannot keep the event loop from polling.
  uint64_t current_iteration_write_iteration_{0};
  // Delivers an injected Write event in the event loop iteration that injected it. Created by the
  // first activation that uses it.
  SchedulableCallbackPtr current_iteration_write_cb_;
};
} // namespace Event
} // namespace Envoy
