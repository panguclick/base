// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/synchronization/waitable_event.h"

#include <mach/mach.h>
#include <sys/event.h>

#include <memory>

#include "base/debug/activity_tracker.h"
#include "base/files/scoped_file.h"
#include "base/mac/mach_logging.h"
#include "base/posix/eintr_wrapper.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "base/time/time_override.h"
#include "build/build_config.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

namespace base {

WaitableEvent::WaitableEvent(ResetPolicy reset_policy,
                             InitialState initial_state)
    : policy_(reset_policy) {
  mach_port_options_t options{};
  options.flags = MPO_INSERT_SEND_RIGHT;
  options.mpl.mpl_qlimit = 1;

  mach_port_t name;
  kern_return_t kr = mach_port_construct(mach_task_self(), &options, 0, &name);
  MACH_CHECK(kr == KERN_SUCCESS, kr) << "mach_port_construct";

  receive_right_ = new ReceiveRight(name);
  send_right_.reset(name);

  if (initial_state == InitialState::SIGNALED)
    Signal();
}

WaitableEvent::~WaitableEvent() = default;

void WaitableEvent::Reset() {
  PeekPort(receive_right_->Name(), true);
}

void WaitableEvent::Signal() {
  mach_msg_empty_send_t msg{};
  msg.header.msgh_bits = MACH_MSGH_BITS_REMOTE(MACH_MSG_TYPE_COPY_SEND);
  msg.header.msgh_size = sizeof(&msg);
  msg.header.msgh_remote_port = send_right_.get();
  // If the event is already signaled, this will time out because the queue
  // has a length of one.
  kern_return_t kr =
      mach_msg(&msg.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(msg), 0,
               MACH_PORT_NULL, 0, MACH_PORT_NULL);
  MACH_CHECK(kr == KERN_SUCCESS || kr == MACH_SEND_TIMED_OUT, kr) << "mach_msg";
}

bool WaitableEvent::IsSignaled() {
  return PeekPort(receive_right_->Name(), policy_ == ResetPolicy::AUTOMATIC);
}

void WaitableEvent::Wait() {
  bool result = TimedWait(TimeDelta::Max());
  DCHECK(result) << "TimedWait() should never fail with infinite timeout";
}

bool WaitableEvent::TimedWait(const TimeDelta& wait_delta) {
  if (wait_delta <= TimeDelta())
    return IsSignaled();

  // Record the event that this thread is blocking upon (for hang diagnosis) and
  // consider blocked for scheduling purposes. Ignore this for non-blocking
  // WaitableEvents.
  absl::optional<debug::ScopedEventWaitActivity> event_activity;
  absl::optional<internal::ScopedBlockingCallWithBaseSyncPrimitives>
      scoped_blocking_call;
  if (waiting_is_blocking_) {
    event_activity.emplace(this);
    scoped_blocking_call.emplace(FROM_HERE, BlockingType::MAY_BLOCK);
  }

  mach_msg_empty_rcv_t msg{};
  msg.header.msgh_local_port = receive_right_->Name();

  mach_msg_option_t options = MACH_RCV_MSG;

  if (!wait_delta.is_max())
    options |= MACH_RCV_TIMEOUT | MACH_RCV_INTERRUPT;

  mach_msg_size_t rcv_size = sizeof(msg);
  if (policy_ == ResetPolicy::MANUAL) {
    // To avoid dequeing the message, receive with a size of 0 and set
    // MACH_RCV_LARGE to keep the message in the queue.
    options |= MACH_RCV_LARGE;
    rcv_size = 0;
  }

  // TimeTicks takes care of overflow but we special case is_max() nonetheless
  // to avoid invoking TimeTicksNowIgnoringOverride() unnecessarily (same for
  // the increment step of the for loop if the condition variable returns
  // early). Ref: https://crbug.com/910524#c7
  const TimeTicks end_time =
      wait_delta.is_max() ? TimeTicks::Max()
                          : subtle::TimeTicksNowIgnoringOverride() + wait_delta;
  // Fake |kr| value to boostrap the for loop.
  kern_return_t kr = MACH_RCV_INTERRUPTED;
  for (mach_msg_timeout_t timeout = wait_delta.is_max()
                                        ? MACH_MSG_TIMEOUT_NONE
                                        : wait_delta.InMillisecondsRoundedUp();
       // If the thread is interrupted during mach_msg(), the system call will
       // be restarted. However, the libsyscall wrapper does not adjust the
       // timeout by the amount of time already waited. Using MACH_RCV_INTERRUPT
       // will instead return from mach_msg(), so that the call can be retried
       // with an adjusted timeout.
       kr == MACH_RCV_INTERRUPTED;
       timeout =
           end_time.is_max()
               ? MACH_MSG_TIMEOUT_NONE
               : std::max<int64_t>(
                     0, (end_time - subtle::TimeTicksNowIgnoringOverride())
                            .InMillisecondsRoundedUp())) {
    kr = mach_msg(&msg.header, options, 0, rcv_size, receive_right_->Name(),
                  timeout, MACH_PORT_NULL);
  }

  if (kr == KERN_SUCCESS) {
    return true;
  } else if (rcv_size == 0 && kr == MACH_RCV_TOO_LARGE) {
    return true;
  } else {
    MACH_CHECK(kr == MACH_RCV_TIMED_OUT, kr) << "mach_msg";
    return false;
  }
}

// static
size_t WaitableEvent::WaitMany(WaitableEvent** raw_waitables, size_t count) {
  DCHECK(count) << "Cannot wait on no events";
  internal::ScopedBlockingCallWithBaseSyncPrimitives scoped_blocking_call(
      FROM_HERE, BlockingType::MAY_BLOCK);
  // Record an event (the first) that this thread is blocking upon.
  debug::ScopedEventWaitActivity event_activity(raw_waitables[0]);

  // On macOS 10.11+, using Mach port sets may cause system instability, per
  // https://crbug.com/756102. On macOS 10.12+, a kqueue can be used
  // instead to work around that.
  enum WaitManyPrimitive {
    KQUEUE,
    PORT_SET,
  };
#if BUILDFLAG(IS_IOS)
  const WaitManyPrimitive kPrimitive = PORT_SET;
#else
  const WaitManyPrimitive kPrimitive = KQUEUE;
#endif
  if (kPrimitive == KQUEUE) {
    std::vector<kevent64_s> events(count);
    for (size_t i = 0; i < count; ++i) {
      EV_SET64(&events[i], raw_waitables[i]->receive_right_->Name(),
               EVFILT_MACHPORT, EV_ADD, 0, 0, i, 0, 0);
    }

    std::vector<kevent64_s> out_events(count);

    ScopedFD wait_many(kqueue());
    PCHECK(wait_many.is_valid()) << "kqueue";

    int rv = HANDLE_EINTR(kevent64(wait_many.get(), events.data(), count,
                                   out_events.data(), count, 0, nullptr));
    PCHECK(rv > 0) << "kevent64";

    size_t triggered = -1;
    for (size_t i = 0; i < static_cast<size_t>(rv); ++i) {
      // WaitMany should return the lowest index in |raw_waitables| that was
      // triggered.
      size_t index = static_cast<size_t>(out_events[i].udata);
      triggered = std::min(triggered, index);
    }

    if (raw_waitables[triggered]->policy_ == ResetPolicy::AUTOMATIC) {
      // The message needs to be dequeued to reset the event.
      PeekPort(raw_waitables[triggered]->receive_right_->Name(), true);
    }

    return triggered;
  } else {
    DCHECK_EQ(kPrimitive, PORT_SET);

    kern_return_t kr;

    mac::ScopedMachPortSet port_set;
    {
      mach_port_t name;
      kr =
          mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &name);
      MACH_CHECK(kr == KERN_SUCCESS, kr) << "mach_port_allocate";
      port_set.reset(name);
    }

    for (size_t i = 0; i < count; ++i) {
      kr = mach_port_insert_member(mach_task_self(),
                                   raw_waitables[i]->receive_right_->Name(),
                                   port_set.get());
      MACH_CHECK(kr == KERN_SUCCESS, kr) << "index " << i;
    }

    mach_msg_empty_rcv_t msg{};
    // Wait on the port set. Only specify space enough for the header, to
    // identify which port in the set is signaled. Otherwise, receiving from the
    // port set may dequeue a message for a manual-reset event object, which
    // would cause it to be reset.
    kr = mach_msg(&msg.header,
                  MACH_RCV_MSG | MACH_RCV_LARGE | MACH_RCV_LARGE_IDENTITY, 0,
                  sizeof(msg.header), port_set.get(), 0, MACH_PORT_NULL);
    MACH_CHECK(kr == MACH_RCV_TOO_LARGE, kr) << "mach_msg";

    for (size_t i = 0; i < count; ++i) {
      WaitableEvent* event = raw_waitables[i];
      if (msg.header.msgh_local_port == event->receive_right_->Name()) {
        if (event->policy_ == ResetPolicy::AUTOMATIC) {
          // The message needs to be dequeued to reset the event.
          PeekPort(msg.header.msgh_local_port, true);
        }
        return i;
      }
    }

    NOTREACHED();
    return 0;
  }
}

// static
bool WaitableEvent::PeekPort(mach_port_t port, bool dequeue) {
  if (dequeue) {
    mach_msg_empty_rcv_t msg{};
    msg.header.msgh_local_port = port;
    kern_return_t kr = mach_msg(&msg.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
                                sizeof(msg), port, 0, MACH_PORT_NULL);
    if (kr == KERN_SUCCESS) {
      return true;
    } else {
      MACH_CHECK(kr == MACH_RCV_TIMED_OUT, kr) << "mach_msg";
      return false;
    }
  } else {
    mach_port_seqno_t seqno = 0;
    mach_msg_size_t size;
    mach_msg_id_t id;
    mach_msg_trailer_t trailer;
    mach_msg_type_number_t trailer_size = sizeof(trailer);
    kern_return_t kr = mach_port_peek(
        mach_task_self(), port, MACH_RCV_TRAILER_TYPE(MACH_RCV_TRAILER_NULL),
        &seqno, &size, &id, reinterpret_cast<mach_msg_trailer_info_t>(&trailer),
        &trailer_size);
    if (kr == KERN_SUCCESS) {
      return true;
    } else {
      MACH_CHECK(kr == KERN_FAILURE, kr) << "mach_port_peek";
      return false;
    }
  }
}

WaitableEvent::ReceiveRight::ReceiveRight(mach_port_t name) : right_(name) {}

WaitableEvent::ReceiveRight::~ReceiveRight() = default;

}  // namespace base
