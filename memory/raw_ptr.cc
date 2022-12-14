// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/memory/raw_ptr.h"
#include <cstdint>

#include "base/allocator/buildflags.h"

// USE_BACKUP_REF_PTR implies USE_PARTITION_ALLOC, needed for code under
// allocator/partition_allocator/ to be built.
#if BUILDFLAG(USE_BACKUP_REF_PTR)

#include "base/allocator/partition_allocator/partition_alloc.h"
#include "base/allocator/partition_allocator/partition_ref_count.h"
#include "base/allocator/partition_allocator/partition_root.h"
#include "base/allocator/partition_allocator/reservation_offset_table.h"
#include "base/check.h"
#include "base/dcheck_is_on.h"

namespace base::internal {

template <bool AllowDangling>
void BackupRefPtrImpl<AllowDangling>::AcquireInternal(uintptr_t address) {
#if DCHECK_IS_ON() || BUILDFLAG(ENABLE_BACKUP_REF_PTR_SLOW_CHECKS)
  CHECK(partition_alloc::IsManagedByPartitionAllocBRPPool(address));
#endif
  uintptr_t slot_start = PartitionAllocGetSlotStartInBRPPool(address);
  if constexpr (AllowDangling)
    PartitionRefCountPointer(slot_start)->AcquireFromUnprotectedPtr();
  else
    PartitionRefCountPointer(slot_start)->Acquire();
}

template <bool AllowDangling>
void BackupRefPtrImpl<AllowDangling>::ReleaseInternal(uintptr_t address) {
#if DCHECK_IS_ON() || BUILDFLAG(ENABLE_BACKUP_REF_PTR_SLOW_CHECKS)
  CHECK(partition_alloc::IsManagedByPartitionAllocBRPPool(address));
#endif
  uintptr_t slot_start = PartitionAllocGetSlotStartInBRPPool(address);
  if constexpr (AllowDangling) {
    if (PartitionRefCountPointer(slot_start)->ReleaseFromUnprotectedPtr())
      PartitionAllocFreeForRefCounting(slot_start);
  } else {
    if (PartitionRefCountPointer(slot_start)->Release())
      PartitionAllocFreeForRefCounting(slot_start);
  }
}

template <bool AllowDangling>
bool BackupRefPtrImpl<AllowDangling>::IsPointeeAlive(uintptr_t address) {
#if DCHECK_IS_ON() || BUILDFLAG(ENABLE_BACKUP_REF_PTR_SLOW_CHECKS)
  CHECK(partition_alloc::IsManagedByPartitionAllocBRPPool(address));
#endif
  uintptr_t slot_start = PartitionAllocGetSlotStartInBRPPool(address);
  return PartitionRefCountPointer(slot_start)->IsAlive();
}

template <bool AllowDangling>
bool BackupRefPtrImpl<AllowDangling>::IsValidDelta(uintptr_t address,
                                                   ptrdiff_t delta_in_bytes) {
  return PartitionAllocIsValidPtrDelta(address, delta_in_bytes);
}

// Explicitly instantiates the two BackupRefPtr variants in the .cc. This
// ensures the definitions not visible from the .h are available in the binary.
template struct BackupRefPtrImpl</*AllowDangling=*/false>;
template struct BackupRefPtrImpl</*AllowDangling=*/true>;

#if DCHECK_IS_ON() || BUILDFLAG(ENABLE_BACKUP_REF_PTR_SLOW_CHECKS)
void CheckThatAddressIsntWithinFirstPartitionPage(uintptr_t address) {
  if (IsManagedByDirectMap(address)) {
    uintptr_t reservation_start = GetDirectMapReservationStart(address);
    CHECK(address - reservation_start >= partition_alloc::PartitionPageSize());
  } else {
    CHECK(IsManagedByNormalBuckets(address));
    CHECK(address % partition_alloc::kSuperPageSize >=
          partition_alloc::PartitionPageSize());
  }
}
#endif  // DCHECK_IS_ON() || BUILDFLAG(ENABLE_BACKUP_REF_PTR_SLOW_CHECKS)

}  // namespace base::internal

#elif BUILDFLAG(USE_ASAN_BACKUP_REF_PTR)

#include <sanitizer/asan_interface.h>
#include "base/logging.h"
#include "base/memory/raw_ptr_asan_service.h"

namespace base::internal {

namespace {
bool IsFreedHeapPointer(void const volatile* ptr) {
  // Use `__asan_region_is_poisoned` instead of `__asan_address_is_poisoned`
  // because the latter may crash on an invalid pointer.
  if (!__asan_region_is_poisoned(const_cast<void*>(ptr), 1))
    return false;

  // Make sure the address is on the heap and is not in a redzone.
  void* region_ptr;
  size_t region_size;
  const char* allocation_type = __asan_locate_address(
      const_cast<void*>(ptr), nullptr, 0, &region_ptr, &region_size);

  auto address = reinterpret_cast<uintptr_t>(ptr);
  auto region_base = reinterpret_cast<uintptr_t>(region_ptr);
  if (strcmp(allocation_type, "heap") != 0 || address < region_base ||
      address >=
          region_base + region_size) {  // We exclude pointers one past the end
                                        // of an allocations from the analysis
                                        // for now because they're to fragile.
    return false;
  }

  // Make sure the allocation has been actually freed rather than
  // user-poisoned.
  int free_thread_id = -1;
  __asan_get_free_stack(region_ptr, nullptr, 0, &free_thread_id);
  return free_thread_id != -1;
}

// Force a non-optimizable memory load operation to trigger an ASan crash.
void ForceRead(void const volatile* ptr) {
  auto unused = *reinterpret_cast<char const volatile*>(ptr);
  asm volatile("" : "+r"(unused));
}
}  // namespace

void AsanBackupRefPtrImpl::AsanCheckIfValidDereference(
    void const volatile* ptr) {
  if (RawPtrAsanService::GetInstance().is_dereference_check_enabled() &&
      IsFreedHeapPointer(ptr)) {
    LOG(ERROR)
        << "BackupRefPtr: Dereferencing a raw_ptr to an already "
           "freed allocation at "
        << const_cast<void*>(ptr)
        << ".\nThis issue is covered by BackupRefPtr in production builds.";
    ForceRead(ptr);
  }
}

void AsanBackupRefPtrImpl::AsanCheckIfValidExtraction(
    void const volatile* ptr) {
  if (RawPtrAsanService::GetInstance().is_extraction_check_enabled() &&
      IsFreedHeapPointer(ptr)) {
    LOG(ERROR)
        << "BackupRefPtr: Extracting from a raw_ptr to an already "
           "freed allocation at "
        << const_cast<void*>(ptr)
        << ".\nIf ASan reports a use-after-free on a related address, it "
           "*may be* covered by BackupRefPtr in production builds but the issue"
           "requires a manual analysis to determine if that's the case.";
    // Don't trigger ASan manually to avoid false-positives when the extracted
    // pointer is never dereferenced.
  }
}

void AsanBackupRefPtrImpl::AsanCheckIfValidInstantiation(
    void const volatile* ptr) {
  if (RawPtrAsanService::GetInstance().is_instantiation_check_enabled() &&
      IsFreedHeapPointer(ptr)) {
    LOG(ERROR) << "BackupRefPtr: Constructing a raw_ptr from a pointer "
                  "to an already freed allocation at "
               << const_cast<void*>(ptr) << " leads to memory corruption.";
    ForceRead(ptr);
  }
}

}  // namespace base::internal

#endif  // BUILDFLAG(USE_ASAN_BACKUP_REF_PTR)
