#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "envoy/event/file_event.h"

#include "source/common/api/os_sys_calls_impl.h"
#include "source/common/event/dispatcher_impl.h"
#include "source/common/event/file_event_impl.h"
#include "source/common/event/schedulable_cb_impl.h"
#include "source/common/memory/stats.h"
#include "source/common/runtime/runtime_features.h"
#include "source/common/stats/isolated_store_impl.h"

#include "test/common/memory/memory_test_utility.h"
#include "test/test_common/environment.h"
#include "test/test_common/test_runtime.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace Envoy {
namespace Event {
namespace {

using ::testing::ElementsAre;
using ::testing::MockFunction;

// True if a Write activation of a file event with the given trigger type is delivered in the event
// loop iteration that made it.
bool writeActivationInCurrentIteration(FileTriggerType trigger) {
  return trigger == FileTriggerType::Edge &&
         Runtime::runtimeFeatureEnabled(
             "envoy.reloadable_features.file_event_write_in_current_iteration");
}

class FileEventImplTest : public testing::Test {
public:
  FileEventImplTest()
      : api_(Api::createApiForTest()), dispatcher_(api_->allocateDispatcher("test_thread")),
        os_sys_calls_(Api::OsSysCallsSingleton::get()) {}

  void SetUp() override {
#ifdef WIN32
    ASSERT_EQ(0, os_sys_calls_.socketpair(AF_INET, SOCK_STREAM, 0, fds_).return_value_);
#else
    ASSERT_EQ(0, os_sys_calls_.socketpair(AF_UNIX, SOCK_DGRAM, 0, fds_).return_value_);
#endif
    int data = 1;

    const Api::SysCallSizeResult result = os_sys_calls_.write(fds_[1], &data, sizeof(data));
    ASSERT_EQ(sizeof(data), static_cast<size_t>(result.return_value_));
  }

  void clearReadable() {
    // Read the data from the socket so it is no longer readable.
    char buffer[10];
    struct iovec vec{buffer, sizeof(buffer)};
    const Api::SysCallSizeResult result = os_sys_calls_.readv(fds_[0], &vec, 1);
    EXPECT_LT(0, static_cast<size_t>(result.return_value_));
    EXPECT_GT(sizeof(buffer), static_cast<size_t>(result.return_value_));
  }

  void TearDown() override {
    os_sys_calls_.close(fds_[0]);
    os_sys_calls_.close(fds_[1]);
  }

protected:
  os_fd_t fds_[2];
  Api::ApiPtr api_;
  DispatcherPtr dispatcher_;
  Api::OsSysCalls& os_sys_calls_;
};

class FileEventImplActivateTest : public testing::TestWithParam<Network::Address::IpVersion> {
public:
  FileEventImplActivateTest() : os_sys_calls_(Api::OsSysCallsSingleton::get()) {}

  static void callPrepareCallback(evwatch*, const evwatch_prepare_cb_info*, void* arg) {
    // `arg` contains the MockFunction passed in from evwatch_prepare_new.
    auto callback = static_cast<MockFunction<void()>*>(arg);
    callback->Call();
  }

  int domain() { return GetParam() == Network::Address::IpVersion::v4 ? AF_INET : AF_INET6; }

protected:
  Api::OsSysCalls& os_sys_calls_;
};

INSTANTIATE_TEST_SUITE_P(IpVersions, FileEventImplActivateTest,
                         testing::ValuesIn(TestEnvironment::getIpVersionsForTest()));

TEST_P(FileEventImplActivateTest, Activate) {
  os_fd_t fd = os_sys_calls_.socket(domain(), SOCK_STREAM, 0).return_value_;
  ASSERT_TRUE(SOCKET_VALID(fd));

  Api::ApiPtr api = Api::createApiForTest();
  DispatcherPtr dispatcher(api->allocateDispatcher("test_thread"));
  MockFunction<void()> read_callback, write_callback;
  EXPECT_CALL(read_callback, Call);
  EXPECT_CALL(write_callback, Call);

  const FileTriggerType trigger = Event::PlatformDefaultTriggerType;

  Event::FileEventPtr file_event = dispatcher->createFileEvent(
      fd,
      [&](uint32_t events) {
        if (events & FileReadyType::Read) {
          read_callback.Call();
        }

        if (events & FileReadyType::Write) {
          write_callback.Call();
        }
        return absl::OkStatus();
      },
      trigger, FileReadyType::Read | FileReadyType::Write);

  file_event->activate(FileReadyType::Read | FileReadyType::Write);
  dispatcher->run(Event::Dispatcher::RunType::NonBlock);

  os_sys_calls_.close(fd);
}

TEST_P(FileEventImplActivateTest, ActivateChaining) {
  os_fd_t fd = os_sys_calls_.socket(domain(), SOCK_DGRAM, 0).return_value_;
  ASSERT_TRUE(SOCKET_VALID(fd));

  Api::ApiPtr api = Api::createApiForTest();
  DispatcherPtr dispatcher(api->allocateDispatcher("test_thread"));
  MockFunction<void()> fd_callback, read_callback, write_callback, prepare_callback;

  evwatch_prepare_new(&static_cast<DispatcherImpl*>(dispatcher.get())->base(), callPrepareCallback,
                      &prepare_callback);

  const FileTriggerType trigger = Event::PlatformDefaultTriggerType;

  Event::FileEventPtr file_event = dispatcher->createFileEvent(
      fd,
      [&](uint32_t events) {
        fd_callback.Call();
        if (events & FileReadyType::Read) {
          read_callback.Call();
          file_event->activate(FileReadyType::Write);
        }

        if (events & FileReadyType::Write) {
          write_callback.Call();
        }
        return absl::OkStatus();
      },
      trigger, FileReadyType::Read | FileReadyType::Write);

  testing::InSequence s;
  // First loop iteration: handle scheduled read event and the real write event produced by poll.
  // Note that the real and injected events are combined and delivered in a single call to the fd
  // callback.
  EXPECT_CALL(prepare_callback, Call);
  EXPECT_CALL(fd_callback, Call);
  EXPECT_CALL(read_callback, Call);
  EXPECT_CALL(write_callback, Call);
  if (writeActivationInCurrentIteration(trigger)) {
    // Still the first loop iteration: handle the write event scheduled while handling read.
    EXPECT_CALL(fd_callback, Call);
    EXPECT_CALL(write_callback, Call);
    // Second loop iteration: poll returned no new real events.
    EXPECT_CALL(prepare_callback, Call);
  } else {
    // Second loop iteration: handle write and close events scheduled while handling read.
    EXPECT_CALL(prepare_callback, Call);
    EXPECT_CALL(fd_callback, Call);
    EXPECT_CALL(write_callback, Call);
    if constexpr (Event::PlatformDefaultTriggerType != Event::FileTriggerType::EmulatedEdge) {
      // Third loop iteration: poll returned no new real events.
      EXPECT_CALL(prepare_callback, Call);
    }
  }

  file_event->activate(FileReadyType::Read);
  dispatcher->run(Event::Dispatcher::RunType::NonBlock);

  os_sys_calls_.close(fd);
}

TEST_P(FileEventImplActivateTest, SetEnableCancelsActivate) {
  os_fd_t fd = os_sys_calls_.socket(domain(), SOCK_DGRAM, 0).return_value_;
  ASSERT_TRUE(SOCKET_VALID(fd));

  Api::ApiPtr api = Api::createApiForTest();
  DispatcherPtr dispatcher(api->allocateDispatcher("test_thread"));
  MockFunction<void()> fd_callback, read_callback, write_callback, prepare_callback;

  evwatch_prepare_new(&static_cast<DispatcherImpl*>(dispatcher.get())->base(), callPrepareCallback,
                      &prepare_callback);

  const FileTriggerType trigger = Event::PlatformDefaultTriggerType;

  Event::FileEventPtr file_event = dispatcher->createFileEvent(
      fd,
      [&](uint32_t events) {
        fd_callback.Call();
        if (events & FileReadyType::Read) {
          read_callback.Call();
          file_event->activate(FileReadyType::Closed);
          file_event->setEnabled(FileReadyType::Write | FileReadyType::Closed);
        }

        if (events & FileReadyType::Write) {
          write_callback.Call();
        }
        return absl::OkStatus();
      },
      trigger, FileReadyType::Read | FileReadyType::Write);

  testing::InSequence s;
  // First loop iteration: handle scheduled read event and the real write event produced by poll.
  // Note that the real and injected events are combined and delivered in a single call to the fd
  // callback.
  EXPECT_CALL(prepare_callback, Call);
  EXPECT_CALL(fd_callback, Call);
  EXPECT_CALL(read_callback, Call);
  EXPECT_CALL(write_callback, Call);
  // Second loop iteration: handle real write event after resetting event mask via setEnabled. Close
  // injected event is discarded by the setEnable call.
  EXPECT_CALL(prepare_callback, Call);
  EXPECT_CALL(fd_callback, Call);
  EXPECT_CALL(write_callback, Call);
  // Third loop iteration: poll returned no new real events.
  EXPECT_CALL(prepare_callback, Call);

  file_event->activate(FileReadyType::Read);
  dispatcher->run(Event::Dispatcher::RunType::NonBlock);

  os_sys_calls_.close(fd);
}

#ifndef WIN32 // Libevent on Windows doesn't support edge trigger.
TEST_F(FileEventImplTest, EdgeTrigger) {
  MockFunction<void()> read_callback, write_callback;
  EXPECT_CALL(read_callback, Call);
  EXPECT_CALL(write_callback, Call);

  Event::FileEventPtr file_event = dispatcher_->createFileEvent(
      fds_[0],
      [&](uint32_t events) {
        if (events & FileReadyType::Read) {
          read_callback.Call();
        }

        if (events & FileReadyType::Write) {
          write_callback.Call();
        }
        return absl::OkStatus();
      },
      FileTriggerType::Edge, FileReadyType::Read | FileReadyType::Write);

  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);
}
#endif

TEST_F(FileEventImplTest, LevelTrigger) {
  testing::InSequence s;
  MockFunction<void()> read_callback, write_callback;

  int count = 0;
  Event::FileEventPtr file_event = dispatcher_->createFileEvent(
      fds_[0],
      [&](uint32_t events) {
        ASSERT(count > 0);
        if (--count == 0) {
          dispatcher_->exit();
        }
        if (events & FileReadyType::Read) {
          read_callback.Call();
        }

        if (events & FileReadyType::Write) {
          write_callback.Call();
        }
        return absl::OkStatus();
      },
      FileTriggerType::Level, FileReadyType::Read | FileReadyType::Write);

  // Expect events to be delivered twice since count=2 and level events are delivered on each
  // iteration until the fd state changes.
  EXPECT_CALL(read_callback, Call);
  EXPECT_CALL(write_callback, Call);
  EXPECT_CALL(read_callback, Call);
  EXPECT_CALL(write_callback, Call);
  count = 2;
  dispatcher_->run(Event::Dispatcher::RunType::Block);

  // Change the event mask to just Write and verify that only that event is delivered.
  EXPECT_CALL(read_callback, Call).Times(0);
  EXPECT_CALL(write_callback, Call);
  file_event->setEnabled(FileReadyType::Write);
  count = 1;
  dispatcher_->run(Event::Dispatcher::RunType::Block);

  // Activate read, and verify it is delivered despite not being part of the enabled event mask.
  EXPECT_CALL(read_callback, Call);
  EXPECT_CALL(write_callback, Call);
  file_event->activate(FileReadyType::Read);
  count = 1;
  dispatcher_->run(Event::Dispatcher::RunType::Block);

  // Activate read and then call setEnabled. Verify that the read event is not delivered; setEnabled
  // clears events from explicit calls to activate.
  EXPECT_CALL(read_callback, Call).Times(0);
  EXPECT_CALL(write_callback, Call);
  file_event->activate(FileReadyType::Read);
  file_event->setEnabled(FileReadyType::Write);
  count = 1;
  dispatcher_->run(Event::Dispatcher::RunType::Block);
}

TEST_F(FileEventImplTest, SetEnabled) {
  testing::InSequence s;
  MockFunction<void()> read_callback, write_callback;

  const FileTriggerType trigger = Event::PlatformDefaultTriggerType;

  Event::FileEventPtr file_event = dispatcher_->createFileEvent(
      fds_[0],
      [&](uint32_t events) {
        if (events & FileReadyType::Read) {
          read_callback.Call();
        }

        if (events & FileReadyType::Write) {
          write_callback.Call();
        }
        return absl::OkStatus();
      },
      trigger, FileReadyType::Read | FileReadyType::Write);

  EXPECT_CALL(read_callback, Call);
  file_event->setEnabled(FileReadyType::Read);
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);

  EXPECT_CALL(write_callback, Call);
  file_event->setEnabled(FileReadyType::Write);
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);

  file_event->setEnabled(0);
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);

  EXPECT_CALL(read_callback, Call);
  EXPECT_CALL(write_callback, Call);
  file_event->setEnabled(FileReadyType::Read | FileReadyType::Write);
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);

  // Ensure that previous expectations are satisfied before moving on.
  testing::Mock::VerifyAndClearExpectations(&read_callback);
  testing::Mock::VerifyAndClearExpectations(&write_callback);

  clearReadable();

  file_event->setEnabled(FileReadyType::Read);
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);

  EXPECT_CALL(write_callback, Call);
  file_event->setEnabled(FileReadyType::Write);
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);

  EXPECT_CALL(write_callback, Call);
  file_event->setEnabled(FileReadyType::Read | FileReadyType::Write);
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);

  // Repeat the previous registration, verify that write event is delivered again.
  EXPECT_CALL(write_callback, Call);
  file_event->setEnabled(FileReadyType::Read | FileReadyType::Write);
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);

  // Synthetic read events are delivered even if the active registration doesn't contain them.
  EXPECT_CALL(read_callback, Call);
  file_event->activate(FileReadyType::Read);
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);

  // Ensure that previous expectations are satisfied before moving on.
  testing::Mock::VerifyAndClearExpectations(&read_callback);
  testing::Mock::VerifyAndClearExpectations(&write_callback);

  // Do a read activation followed setEnabled to verify that the activation is cleared.
  EXPECT_CALL(write_callback, Call);
  file_event->activate(FileReadyType::Read);
  file_event->setEnabled(FileReadyType::Read | FileReadyType::Write);
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);

  // Repeat the previous steps but with the same input to setEnabled to verify that the activation
  // is cleared even in cases where the setEnable mask hasn't changed.
  EXPECT_CALL(write_callback, Call);
  file_event->activate(FileReadyType::Read);
  file_event->setEnabled(FileReadyType::Read | FileReadyType::Write);
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);
}

#ifndef WIN32 // Libevent on Windows doesn't support edge trigger.
// envoy.reloadable_features.file_event_write_in_current_iteration, with the guard on and off: when
// an injected Write event is delivered relative to the polls for fd events and to other callbacks
// of the event loop.
class FileEventImplWriteActivationTest : public testing::TestWithParam<bool> {
public:
  FileEventImplWriteActivationTest() : os_sys_calls_(Api::OsSysCallsSingleton::get()) {
    scoped_runtime_.mergeValues({{"envoy.reloadable_features.file_event_write_in_current_iteration",
                                  guardEnabled() ? "true" : "false"}});
    api_ = Api::createApiForTest();
    dispatcher_ = api_->allocateDispatcher("test_thread");
    evwatch_prepare_new(&static_cast<DispatcherImpl*>(dispatcher_.get())->base(), logPoll, &log_);
  }

  void SetUp() override {
    ASSERT_EQ(0, os_sys_calls_.socketpair(AF_UNIX, SOCK_STREAM, 0, fds_).return_value_);
  }

  void TearDown() override {
    file_event_.reset();
    os_sys_calls_.close(fds_[0]);
    os_sys_calls_.close(fds_[1]);
  }

  bool guardEnabled() const { return GetParam(); }

  static void logPoll(evwatch*, const evwatch_prepare_cb_info*, void* arg) {
    static_cast<std::vector<std::string>*>(arg)->push_back("poll");
  }

  static std::string eventsToString(uint32_t events) {
    std::string result;
    if (events & FileReadyType::Read) {
      result += "read";
    }
    if (events & FileReadyType::Write) {
      result += "write";
    }
    if (events & FileReadyType::Closed) {
      result += "closed";
    }
    return result;
  }

  // Creates the file event on one end of a connected socket pair and consumes the events that the
  // registration itself produces, so that what is logged afterwards comes from the test.
  void createFileEvent(FileTriggerType trigger, uint32_t events,
                       std::function<void(uint32_t)> on_event = nullptr) {
    file_event_ = dispatcher_->createFileEvent(
        fds_[0],
        [this, on_event](uint32_t events) {
          log_.push_back(eventsToString(events));
          if (on_event != nullptr && registration_events_consumed_) {
            on_event(events);
          }
          return absl::OkStatus();
        },
        trigger, events);
    if (trigger == FileTriggerType::Edge) {
      dispatcher_->run(Dispatcher::RunType::NonBlock);
    }
    registration_events_consumed_ = true;
    log_.clear();
  }

  // Makes the file event's end of the socket pair readable.
  void makeReadable() {
    const char data = 'x';
    ASSERT_EQ(1, os_sys_calls_.write(fds_[1], &data, 1).return_value_);
  }

  // Runs the event loop until it has nothing left to do. `in_loop` runs as the first event of the
  // first iteration: scheduled before the event loop starts, it is on the work list ahead of the
  // fd events that the first poll finds.
  void runInLoop(std::function<void()> in_loop) {
    SchedulableCallbackPtr first = dispatcher_->createSchedulableCallback(in_loop);
    first->scheduleCallbackCurrentIteration();
    dispatcher_->run(Dispatcher::RunType::NonBlock);
  }

  // A callback that logs `name` when it runs.
  SchedulableCallbackPtr logger(const std::string& name) {
    return dispatcher_->createSchedulableCallback([this, name]() { log_.push_back(name); });
  }

protected:
  TestScopedRuntime scoped_runtime_;
  Api::OsSysCalls& os_sys_calls_;
  Api::ApiPtr api_;
  DispatcherPtr dispatcher_;
  os_fd_t fds_[2];
  std::vector<std::string> log_;
  bool registration_events_consumed_{false};
  FileEventPtr file_event_;
};

INSTANTIATE_TEST_SUITE_P(Guard, FileEventImplWriteActivationTest, testing::Bool());

// With the guard, a Write activation of an edge-triggered file event is delivered in the iteration
// that made it: after the callbacks that were scheduled before it, before the ones scheduled after
// it, and never inside activate(). Without the guard it is delivered after the next poll.
TEST_P(FileEventImplWriteActivationTest, DeliveredInTheIterationThatMadeIt) {
  createFileEvent(FileTriggerType::Edge, FileReadyType::Read | FileReadyType::Write);
  SchedulableCallbackPtr before = logger("before");
  SchedulableCallbackPtr after = logger("after");

  runInLoop([&]() {
    before->scheduleCallbackCurrentIteration();
    file_event_->activate(FileReadyType::Write);
    log_.push_back("activated");
    after->scheduleCallbackCurrentIteration();
  });

  if (guardEnabled()) {
    EXPECT_THAT(log_, ElementsAre("poll", "activated", "before", "write", "after", "poll"));
  } else {
    EXPECT_THAT(log_, ElementsAre("poll", "activated", "before", "after", "poll", "write", "poll"));
  }
}

// Several Write activations before the delivery are one delivery.
TEST_P(FileEventImplWriteActivationTest, ActivationsBeforeTheDeliveryAreOneDelivery) {
  createFileEvent(FileTriggerType::Edge, FileReadyType::Read | FileReadyType::Write);
  SchedulableCallbackPtr second = dispatcher_->createSchedulableCallback([&]() {
    log_.push_back("second");
    file_event_->activate(FileReadyType::Write);
  });

  runInLoop([&]() {
    file_event_->activate(FileReadyType::Write);
    file_event_->activate(FileReadyType::Write);
    // Scheduled after the delivery of the Write event, so with the guard this one runs after it.
    second->scheduleCallbackCurrentIteration();
  });

  if (guardEnabled()) {
    // The activation made after the delivery is the second one of the iteration.
    EXPECT_THAT(log_, ElementsAre("poll", "write", "second", "poll", "write", "poll"));
  } else {
    EXPECT_THAT(log_, ElementsAre("poll", "second", "poll", "write", "poll"));
  }
}

// A file event gets at most one injected Write event per iteration: a callback that activates
// Write whenever it runs is called once per poll, with and without the guard.
TEST_P(FileEventImplWriteActivationTest, OncePerIteration) {
  int deliveries = 0;
  createFileEvent(FileTriggerType::Edge, FileReadyType::Read | FileReadyType::Write, [&](uint32_t) {
    if (++deliveries < 4) {
      file_event_->activate(FileReadyType::Write);
    }
  });

  runInLoop([&]() { file_event_->activate(FileReadyType::Write); });

  if (guardEnabled()) {
    EXPECT_THAT(log_, ElementsAre("poll", "write", "poll", "write", "poll", "write", "poll",
                                  "write", "poll"));
  } else {
    EXPECT_THAT(log_, ElementsAre("poll", "poll", "write", "poll", "write", "poll", "write", "poll",
                                  "write", "poll"));
  }
  EXPECT_EQ(4, deliveries);
}

// The first Write activation of a later iteration is delivered in that iteration again.
TEST_P(FileEventImplWriteActivationTest, EveryIterationHasItsOwnDelivery) {
  createFileEvent(FileTriggerType::Edge, FileReadyType::Read | FileReadyType::Write);
  for (int i = 0; i < 3; i++) {
    log_.clear();
    SchedulableCallbackPtr after = logger("after");
    runInLoop([&]() {
      file_event_->activate(FileReadyType::Write);
      after->scheduleCallbackCurrentIteration();
    });
    if (guardEnabled()) {
      EXPECT_THAT(log_, ElementsAre("poll", "write", "after", "poll"));
    } else {
      EXPECT_THAT(log_, ElementsAre("poll", "after", "poll", "write", "poll"));
    }
  }
}

// A Read activation is never delivered in the iteration that made it, and is not merged into the
// delivery of a Write event that is.
TEST_P(FileEventImplWriteActivationTest, ReadActivationAfterWriteActivation) {
  createFileEvent(FileTriggerType::Edge, FileReadyType::Read | FileReadyType::Write);

  runInLoop([&]() {
    file_event_->activate(FileReadyType::Write);
    file_event_->activate(FileReadyType::Read);
  });

  if (guardEnabled()) {
    EXPECT_THAT(log_, ElementsAre("poll", "write", "poll", "read", "poll"));
  } else {
    EXPECT_THAT(log_, ElementsAre("poll", "poll", "readwrite", "poll"));
  }
}

// A Write activation while another injected event is pending joins that event, as before: the next
// iteration does not wait for fd events anyway.
TEST_P(FileEventImplWriteActivationTest, WriteActivationAfterReadActivation) {
  createFileEvent(FileTriggerType::Edge, FileReadyType::Read | FileReadyType::Write);

  runInLoop([&]() {
    file_event_->activate(FileReadyType::Read);
    file_event_->activate(FileReadyType::Write);
  });

  EXPECT_THAT(log_, ElementsAre("poll", "poll", "readwrite", "poll"));
}

// Only an activation of exactly Write is delivered in the iteration that made it.
TEST_P(FileEventImplWriteActivationTest, ReadAndWriteActivatedTogether) {
  createFileEvent(FileTriggerType::Edge, FileReadyType::Read | FileReadyType::Write);

  runInLoop([&]() { file_event_->activate(FileReadyType::Read | FileReadyType::Write); });
  EXPECT_THAT(log_, ElementsAre("poll", "poll", "readwrite", "poll"));

  log_.clear();
  runInLoop([&]() { file_event_->activate(FileReadyType::Write | FileReadyType::Closed); });
  EXPECT_THAT(log_, ElementsAre("poll", "poll", "writeclosed", "poll"));
}

// An injected Write event that was delivered through the next-iteration path counts as the Write
// event of that iteration: an activation made while it is handled waits for the next iteration.
TEST_P(FileEventImplWriteActivationTest, ActivationWhileHandlingNextIterationDelivery) {
  bool again = true;
  createFileEvent(FileTriggerType::Edge, FileReadyType::Read | FileReadyType::Write,
                  [&](uint32_t events) {
                    if (events == (FileReadyType::Read | FileReadyType::Write) && again) {
                      again = false;
                      file_event_->activate(FileReadyType::Write);
                    }
                  });

  runInLoop([&]() { file_event_->activate(FileReadyType::Read | FileReadyType::Write); });

  EXPECT_THAT(log_, ElementsAre("poll", "poll", "readwrite", "poll", "write", "poll"));
}

// setEnabled() drops a Write event that is scheduled for the current iteration, as it drops the
// injected events of the next iteration. Registering an edge-triggered file event again reports
// the state of the fd, so a writable fd still gets its Write event, from the next poll.
TEST_P(FileEventImplWriteActivationTest, SetEnabledDropsScheduledWrite) {
  createFileEvent(FileTriggerType::Edge, FileReadyType::Read | FileReadyType::Write);

  runInLoop([&]() {
    file_event_->activate(FileReadyType::Write);
    file_event_->setEnabled(FileReadyType::Read | FileReadyType::Write);
  });
  EXPECT_THAT(log_, ElementsAre("poll", "poll", "write", "poll"));

  // Without Write in the new registration nothing is delivered.
  log_.clear();
  runInLoop([&]() {
    file_event_->activate(FileReadyType::Write);
    file_event_->setEnabled(FileReadyType::Read);
  });
  EXPECT_THAT(log_, ElementsAre("poll", "poll"));

  // The file event can deliver a Write event in the iteration again afterwards.
  log_.clear();
  runInLoop([&]() { file_event_->activate(FileReadyType::Write); });
  if (guardEnabled()) {
    EXPECT_THAT(log_, ElementsAre("poll", "write", "poll"));
  } else {
    EXPECT_THAT(log_, ElementsAre("poll", "poll", "write", "poll"));
  }
}

// A Write activation that follows setEnabled() in the same iteration is delivered in the next one:
// the file event already had its Write event of this iteration scheduled.
TEST_P(FileEventImplWriteActivationTest, ActivationAfterSetEnabledInTheSameIteration) {
  createFileEvent(FileTriggerType::Edge, FileReadyType::Read);

  runInLoop([&]() {
    file_event_->activate(FileReadyType::Write);
    file_event_->setEnabled(FileReadyType::Read);
    file_event_->activate(FileReadyType::Write);
  });
  EXPECT_THAT(log_, ElementsAre("poll", "poll", "write", "poll"));
}

// A real event of the fd that is handled while a Write event is scheduled for the current
// iteration delivers that Write event too, as it delivers the injected events of the next
// iteration, and the scheduled delivery does not run a second time.
TEST_P(FileEventImplWriteActivationTest, RealEventDeliversScheduledWrite) {
  // Not registered for Write, so that a real event carries Read only.
  createFileEvent(FileTriggerType::Edge, FileReadyType::Read);
  makeReadable();
  SchedulableCallbackPtr after = logger("after");

  // The first poll finds the fd readable. Its event is handled after this callback.
  runInLoop([&]() {
    file_event_->activate(FileReadyType::Write);
    after->scheduleCallbackCurrentIteration();
  });

  EXPECT_THAT(log_, ElementsAre("poll", "readwrite", "after", "poll"));
}

// Destroying the file event drops a Write event that is scheduled for the current iteration.
TEST_P(FileEventImplWriteActivationTest, DestroyedBeforeTheDelivery) {
  createFileEvent(FileTriggerType::Edge, FileReadyType::Read | FileReadyType::Write);
  SchedulableCallbackPtr after = logger("after");

  runInLoop([&]() {
    file_event_->activate(FileReadyType::Write);
    after->scheduleCallbackCurrentIteration();
    file_event_.reset();
  });

  // With the file event gone nothing is registered with the event loop, so it does not poll again.
  EXPECT_THAT(log_, ElementsAre("poll", "after"));
}

// The callback may destroy the file event while it handles the Write event.
TEST_P(FileEventImplWriteActivationTest, DestroyedByTheDelivery) {
  createFileEvent(FileTriggerType::Edge, FileReadyType::Read | FileReadyType::Write,
                  [&](uint32_t) { file_event_.reset(); });

  runInLoop([&]() { file_event_->activate(FileReadyType::Write); });

  // With the file event gone nothing is registered with the event loop, so it does not poll again.
  if (guardEnabled()) {
    EXPECT_THAT(log_, ElementsAre("poll", "write"));
  } else {
    EXPECT_THAT(log_, ElementsAre("poll", "poll", "write"));
  }
  EXPECT_EQ(nullptr, file_event_);
}

// A Write activation made before the event loop runs is delivered by its first iteration.
TEST_P(FileEventImplWriteActivationTest, ActivatedOutsideTheEventLoop) {
  createFileEvent(FileTriggerType::Edge, FileReadyType::Read | FileReadyType::Write);

  file_event_->activate(FileReadyType::Write);
  EXPECT_TRUE(log_.empty());
  dispatcher_->run(Dispatcher::RunType::NonBlock);
  EXPECT_THAT(log_, ElementsAre("poll", "write", "poll"));

  // The same with nothing else to do in between: every run of the event loop is a new iteration.
  log_.clear();
  file_event_->activate(FileReadyType::Write);
  dispatcher_->run(Dispatcher::RunType::NonBlock);
  EXPECT_THAT(log_, ElementsAre("poll", "write", "poll"));
}

// A Write event that was scheduled before the event loop ran is the injected Write event of the
// iteration that delivers it: an activation made while it is handled waits for the next iteration.
TEST_P(FileEventImplWriteActivationTest, ActivatedOutsideTheEventLoopAndAgainByTheDelivery) {
  int deliveries = 0;
  createFileEvent(FileTriggerType::Edge, FileReadyType::Read | FileReadyType::Write, [&](uint32_t) {
    if (++deliveries < 3) {
      file_event_->activate(FileReadyType::Write);
    }
  });

  file_event_->activate(FileReadyType::Write);
  dispatcher_->run(Dispatcher::RunType::NonBlock);
  EXPECT_THAT(log_, ElementsAre("poll", "write", "poll", "write", "poll", "write", "poll"));
}

// The event loop exits after the iteration in which exit() was called. A Write event scheduled for
// that iteration is still delivered.
TEST_P(FileEventImplWriteActivationTest, DeliveredBeforeTheEventLoopExits) {
  createFileEvent(FileTriggerType::Edge, FileReadyType::Read | FileReadyType::Write);

  SchedulableCallbackPtr first = dispatcher_->createSchedulableCallback([&]() {
    file_event_->activate(FileReadyType::Write);
    dispatcher_->exit();
  });
  first->scheduleCallbackCurrentIteration();
  dispatcher_->run(Dispatcher::RunType::RunUntilExit);

  if (guardEnabled()) {
    EXPECT_THAT(log_, ElementsAre("poll", "write"));
  } else {
    // The Write event is due after the next poll, which does not happen.
    EXPECT_THAT(log_, ElementsAre("poll"));
  }
}

// Level-triggered file events keep the delivery in the next iteration.
TEST_P(FileEventImplWriteActivationTest, LevelTriggeredIsDeliveredInTheNextIteration) {
  // Not registered for Write: a level-triggered Write event of a writable fd would fire in every
  // iteration.
  createFileEvent(FileTriggerType::Level, FileReadyType::Read);
  SchedulableCallbackPtr after = logger("after");

  runInLoop([&]() {
    file_event_->activate(FileReadyType::Write);
    after->scheduleCallbackCurrentIteration();
  });

  EXPECT_THAT(log_, ElementsAre("poll", "after", "poll", "write", "poll"));
}

// Two file events whose Write callbacks activate each other, as two connections that relay to
// each other do: each file event gets at most one injected Write event per iteration, so the pair
// cannot keep the event loop from polling. With the guard, each iteration delivers both once; the
// activation that comes back to a file event that already had its Write event waits for the next
// iteration.
TEST_P(FileEventImplWriteActivationTest, TwoFileEventsThatActivateEachOther) {
  os_fd_t other_fds[2];
  ASSERT_EQ(0, os_sys_calls_.socketpair(AF_UNIX, SOCK_STREAM, 0, other_fds).return_value_);
  int deliveries = 0;
  FileEventPtr first;
  FileEventPtr second;
  auto relay = [&](const std::string& name, FileEventPtr& other) {
    return [&, name](uint32_t events) {
      EXPECT_EQ(FileReadyType::Write, events);
      log_.push_back(name);
      if (++deliveries < 6) {
        other->activate(FileReadyType::Write);
      }
      return absl::OkStatus();
    };
  };
  // Registered for Read only, on sockets that are not readable, so that only activations deliver.
  first = dispatcher_->createFileEvent(fds_[0], relay("first", second), FileTriggerType::Edge,
                                       FileReadyType::Read);
  second = dispatcher_->createFileEvent(other_fds[0], relay("second", first), FileTriggerType::Edge,
                                        FileReadyType::Read);
  dispatcher_->run(Dispatcher::RunType::NonBlock);
  log_.clear();

  runInLoop([&]() { first->activate(FileReadyType::Write); });

  if (guardEnabled()) {
    EXPECT_THAT(log_, ElementsAre("poll", "first", "second", "poll", "first", "second", "poll",
                                  "first", "second", "poll"));
  } else {
    EXPECT_THAT(log_, ElementsAre("poll", "poll", "first", "poll", "second", "poll", "first",
                                  "poll", "second", "poll", "first", "poll", "second", "poll"));
  }
  EXPECT_EQ(6, deliveries);

  first.reset();
  second.reset();
  os_sys_calls_.close(other_fds[0]);
  os_sys_calls_.close(other_fds[1]);
}

// The memory that the guard costs. Every FileEventImpl has three more members (24 bytes on LP64)
// whether the guard is on or off. With the guard on, the first Write activation that takes the
// new path also allocates the SchedulableCallbackImpl that delivers it, with its std::function,
// which the file event keeps until it is destroyed; with the guard off nothing is allocated per
// activation. A connection has
// one file event, so these are the costs per connection socket. The figures are measured over many
// file events, so that the allocator's own variation is spread out, and logged.
TEST_P(FileEventImplWriteActivationTest, MemoryPerFileEvent) {
  if (Memory::TestUtil::MemoryTest::mode() == Memory::TestUtil::MemoryTest::Mode::Disabled) {
    GTEST_SKIP() << "the heap use of this build cannot be measured";
  }
  constexpr int64_t kFileEvents = 1000;
  std::vector<FileEventPtr> file_events;
  file_events.reserve(kFileEvents);
  log_.reserve(64);
  int64_t deliveries = 0;
  auto allocated = []() { return static_cast<int64_t>(Memory::Stats::totalCurrentlyAllocated()); };

  const int64_t before_create = allocated();
  for (int64_t i = 0; i < kFileEvents; i++) {
    // Registered for Read only, on a socket that is not readable, so that only activations
    // deliver.
    file_events.push_back(dispatcher_->createFileEvent(
        fds_[0],
        [&deliveries](uint32_t) {
          deliveries++;
          return absl::OkStatus();
        },
        FileTriggerType::Edge, FileReadyType::Read));
  }
  const int64_t created = allocated() - before_create;
  dispatcher_->run(Dispatcher::RunType::NonBlock);

  const int64_t before_write = allocated();
  runInLoop([&]() {
    for (FileEventPtr& file_event : file_events) {
      file_event->activate(FileReadyType::Write);
    }
  });
  ASSERT_EQ(kFileEvents, deliveries);
  const int64_t written = allocated() - before_write;

  // A second round reuses what the first one allocated.
  const int64_t before_second_write = allocated();
  runInLoop([&]() {
    for (FileEventPtr& file_event : file_events) {
      file_event->activate(FileReadyType::Write);
    }
  });
  ASSERT_EQ(2 * kFileEvents, deliveries);
  const int64_t written_again = allocated() - before_second_write;

  ENVOY_LOG_MISC(info,
                 "guard {}: sizeof(FileEventImpl) {}, sizeof(SchedulableCallbackImpl) {}; heap "
                 "bytes per file event: created {}, first Write activation {}, second {}",
                 guardEnabled() ? "on" : "off", sizeof(FileEventImpl),
                 sizeof(SchedulableCallbackImpl), created / kFileEvents, written / kFileEvents,
                 written_again / kFileEvents);
  RecordProperty("heap_bytes_per_file_event_created", static_cast<int>(created / kFileEvents));
  RecordProperty("heap_bytes_per_file_event_first_write", static_cast<int>(written / kFileEvents));

  // Creating a file event allocates the file event and its next-iteration callback.
  EXPECT_GE(created, kFileEvents * static_cast<int64_t>(sizeof(FileEventImpl) +
                                                        sizeof(SchedulableCallbackImpl)));
  if (guardEnabled()) {
    // One SchedulableCallbackImpl per file event, and the heap block of the std::function in it:
    // DispatcherImpl::createSchedulableCallback() wraps the callback in a lambda that holds the
    // dispatcher and a std::function, which is too large for the inline buffer of std::function.
    // Both are rounded up to the allocator's size classes.
    EXPECT_GE(written, kFileEvents * static_cast<int64_t>(sizeof(SchedulableCallbackImpl)));
    EXPECT_LE(written, kFileEvents * static_cast<int64_t>(sizeof(SchedulableCallbackImpl) + 128));
  } else {
    // Nothing per file event: the next-iteration path only grows libevent's shared timer heap.
    EXPECT_LT(written, kFileEvents * 32);
  }
  // Nothing more per file event in later iterations, in either arm.
  EXPECT_LT(written_again, kFileEvents * 8);
}
#endif

TEST_F(FileEventImplTest, RegisterIfEmulatedEdge) {
  // Test only applies if using EmulatedEdge trigger mode
  if constexpr (PlatformDefaultTriggerType != FileTriggerType::EmulatedEdge) {
    return;
  }

  testing::InSequence s;
  MockFunction<void()> read_callback, write_callback;

  const FileTriggerType trigger = Event::PlatformDefaultTriggerType;

  Event::FileEventPtr file_event = dispatcher_->createFileEvent(
      fds_[0],
      [&](uint32_t events) {
        if (events & FileReadyType::Read) {
          read_callback.Call();
        }

        if (events & FileReadyType::Write) {
          write_callback.Call();
        }
        return absl::OkStatus();
      },
      trigger, FileReadyType::Read | FileReadyType::Write);

  EXPECT_CALL(read_callback, Call).Times(0);
  EXPECT_CALL(write_callback, Call).Times(0);
  file_event->unregisterEventIfEmulatedEdge(FileReadyType::Read | FileReadyType::Write);
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);

  EXPECT_CALL(read_callback, Call);
  file_event->registerEventIfEmulatedEdge(FileReadyType::Read);
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);

  EXPECT_CALL(write_callback, Call);
  file_event->registerEventIfEmulatedEdge(FileReadyType::Write);
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);

  EXPECT_CALL(read_callback, Call);
  file_event->registerEventIfEmulatedEdge(FileReadyType::Read | FileReadyType::Write);
  file_event->unregisterEventIfEmulatedEdge(FileReadyType::Write);
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);

  EXPECT_CALL(read_callback, Call).Times(0);
  EXPECT_CALL(write_callback, Call).Times(0);
  file_event->unregisterEventIfEmulatedEdge(FileReadyType::Read);
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);

  EXPECT_CALL(read_callback, Call);
  EXPECT_CALL(write_callback, Call);
  file_event->registerEventIfEmulatedEdge(FileReadyType::Read | FileReadyType::Write);
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);

  // Events are delivered once due to auto unregistration after they are delivered.
  EXPECT_CALL(read_callback, Call).Times(0);
  EXPECT_CALL(write_callback, Call).Times(0);
  dispatcher_->run(Event::Dispatcher::RunType::NonBlock);
}

} // namespace
} // namespace Event
} // namespace Envoy
