// Copyright 2011 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_SYNCHRONIZATION_LOCK_IMPL_H_
#define BASE_SYNCHRONIZATION_LOCK_IMPL_H_

#include "base/base_export.h"
#include "base/check.h"
#include "base/dcheck_is_on.h"
#include "base/thread_annotations.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_WIN)
#include "base/win/windows_types.h"
#elif BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
#include <errno.h>
#include <pthread.h>
#include <string.h>
#endif

namespace base {
class Lock;
class ConditionVariable;

namespace win {
namespace internal {
class AutoNativeLock;
class ScopedHandleVerifier;
}  // namespace internal
}  // namespace win

namespace internal {

// This class implements the underlying platform-specific spin-lock mechanism
// used for the Lock class. Do not use, use Lock instead.
class BASE_EXPORT LockImpl {
 public:
  LockImpl(const LockImpl&) = delete;
  LockImpl& operator=(const LockImpl&) = delete;

 private:
  friend class base::Lock;
  friend class base::ConditionVariable;
  friend class base::win::internal::AutoNativeLock;
  friend class base::win::internal::ScopedHandleVerifier;

#if BUILDFLAG(IS_WIN)
  using NativeHandle = CHROME_SRWLOCK;
#elif BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
  using NativeHandle = pthread_mutex_t;
#endif

  LockImpl();
  ~LockImpl();

  // If the lock is not held, take it and return true.  If the lock is already
  // held by something else, immediately return false.
  inline bool Try();

  // Take the lock, blocking until it is available if necessary.
  inline void Lock();

  // Release the lock.  This must only be called by the lock's holder: after
  // a successful call to Try, or a call to Lock.
  inline void Unlock();

  // Return the native underlying lock.
  // TODO(awalker): refactor lock and condition variables so that this is
  // unnecessary.
  NativeHandle* native_handle() { return &native_handle_; }

#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)
  // Whether this lock will attempt to use priority inheritance.
  static bool PriorityInheritanceAvailable();
#endif

  void LockInternalWithTracking();
  NativeHandle native_handle_;
};

void LockImpl::Lock() {
  // The ScopedLockAcquireActivity in LockInternalWithTracking() (not inlined
  // here because of circular includes) is relatively expensive and so its
  // actions can become significant due to the very large number of locks that
  // tend to be used throughout the build. It is also not needed unless the lock
  // is contended.
  //
  // To avoid this cost in the vast majority of the calls, simply "try" the lock
  // first and only do the (tracked) blocking call if that fails. |Try()| is
  // cheap on platforms with futex-type locks, as it doesn't call into the
  // kernel.
  if (LIKELY(Try()))
    return;

  LockInternalWithTracking();
}

#if BUILDFLAG(IS_WIN)
bool LockImpl::Try() {
  return !!::TryAcquireSRWLockExclusive(
      reinterpret_cast<PSRWLOCK>(&native_handle_));
}

void LockImpl::Unlock() {
  ::ReleaseSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&native_handle_));
}

#elif BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_FUCHSIA)

#if DCHECK_IS_ON()
BASE_EXPORT void dcheck_trylock_result(int rv);
BASE_EXPORT void dcheck_unlock_result(int rv);
#endif

bool LockImpl::Try() {
  int rv = pthread_mutex_trylock(&native_handle_);
#if DCHECK_IS_ON()
  dcheck_trylock_result(rv);
#endif
  return rv == 0;
}

void LockImpl::Unlock() {
  [[maybe_unused]] int rv = pthread_mutex_unlock(&native_handle_);
#if DCHECK_IS_ON()
  dcheck_unlock_result(rv);
#endif
}
#endif

// This is an implementation used for AutoLock templated on the lock type.
template <class LockType>
class SCOPED_LOCKABLE BasicAutoLock {
 public:
  struct AlreadyAcquired {};

  explicit BasicAutoLock(LockType& lock) EXCLUSIVE_LOCK_FUNCTION(lock)
      : lock_(lock) {
    lock_.Acquire();
  }

  BasicAutoLock(LockType& lock, const AlreadyAcquired&)
      EXCLUSIVE_LOCKS_REQUIRED(lock)
      : lock_(lock) {
    lock_.AssertAcquired();
  }

  BasicAutoLock(const BasicAutoLock&) = delete;
  BasicAutoLock& operator=(const BasicAutoLock&) = delete;

  ~BasicAutoLock() UNLOCK_FUNCTION() {
    lock_.AssertAcquired();
    lock_.Release();
  }

 private:
  LockType& lock_;
};

// This is an implementation used for AutoTryLock templated on the lock type.
template <class LockType>
class SCOPED_LOCKABLE BasicAutoTryLock {
 public:
  explicit BasicAutoTryLock(LockType& lock) EXCLUSIVE_LOCK_FUNCTION(lock)
      : lock_(lock), is_acquired_(lock_.Try()) {}

  BasicAutoTryLock(const BasicAutoTryLock&) = delete;
  BasicAutoTryLock& operator=(const BasicAutoTryLock&) = delete;

  ~BasicAutoTryLock() UNLOCK_FUNCTION() {
    if (is_acquired_) {
      lock_.AssertAcquired();
      lock_.Release();
    }
  }

  bool is_acquired() const { return is_acquired_; }

 private:
  LockType& lock_;
  const bool is_acquired_;
};

// This is an implementation used for AutoUnlock templated on the lock type.
template <class LockType>
class BasicAutoUnlock {
 public:
  explicit BasicAutoUnlock(LockType& lock) : lock_(lock) {
    // We require our caller to have the lock.
    lock_.AssertAcquired();
    lock_.Release();
  }

  BasicAutoUnlock(const BasicAutoUnlock&) = delete;
  BasicAutoUnlock& operator=(const BasicAutoUnlock&) = delete;

  ~BasicAutoUnlock() { lock_.Acquire(); }

 private:
  LockType& lock_;
};

// This is an implementation used for AutoLockMaybe templated on the lock type.
template <class LockType>
class SCOPED_LOCKABLE BasicAutoLockMaybe {
 public:
  explicit BasicAutoLockMaybe(LockType* lock) EXCLUSIVE_LOCK_FUNCTION(lock)
      : lock_(lock) {
    if (lock_)
      lock_->Acquire();
  }

  BasicAutoLockMaybe(const BasicAutoLockMaybe&) = delete;
  BasicAutoLockMaybe& operator=(const BasicAutoLockMaybe&) = delete;

  ~BasicAutoLockMaybe() UNLOCK_FUNCTION() {
    if (lock_) {
      lock_->AssertAcquired();
      lock_->Release();
    }
  }

 private:
  LockType* const lock_;
};

// This is an implementation used for ReleasableAutoLock templated on the lock
// type.
template <class LockType>
class SCOPED_LOCKABLE BasicReleasableAutoLock {
 public:
  explicit BasicReleasableAutoLock(LockType* lock) EXCLUSIVE_LOCK_FUNCTION(lock)
      : lock_(lock) {
    DCHECK(lock_);
    lock_->Acquire();
  }

  BasicReleasableAutoLock(const BasicReleasableAutoLock&) = delete;
  BasicReleasableAutoLock& operator=(const BasicReleasableAutoLock&) = delete;

  ~BasicReleasableAutoLock() UNLOCK_FUNCTION() {
    if (lock_) {
      lock_->AssertAcquired();
      lock_->Release();
    }
  }

  void Release() UNLOCK_FUNCTION() {
    DCHECK(lock_);
    lock_->AssertAcquired();
    lock_->Release();
    lock_ = nullptr;
  }

 private:
  LockType* lock_;
};

}  // namespace internal
}  // namespace base

#endif  // BASE_SYNCHRONIZATION_LOCK_IMPL_H_
