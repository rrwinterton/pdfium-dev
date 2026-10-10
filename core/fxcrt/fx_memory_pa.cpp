// Copyright 2022 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "core/fxcrt/fx_memory.h"

#include <array>

#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/fx_memcpy_wrappers.h"
#include "core/fxcrt/fx_safe_types.h"

#if defined(PDF_USE_PARTITION_ALLOC)
#include "partition_alloc/partition_alloc.h"
#else
#error "File compiled under wrong build option."
#endif

namespace {

constexpr partition_alloc::PartitionOptions kOptions = {};

partition_alloc::PartitionAllocator& GetGeneralPartitionAllocator() {
  static auto* general_allocator =
      new partition_alloc::PartitionAllocator{kOptions};
  return *general_allocator;
}

partition_alloc::PartitionAllocator& GetStringPartitionAllocator() {
  static auto* string_allocator =
      new partition_alloc::PartitionAllocator{kOptions};
  return *string_allocator;
}

// PartitionAlloc direct-maps allocations above its largest bucket (960 KiB)
// and returns them to the OS as soon as they are freed, so callers that
// repeatedly allocate and free multi-megabyte buffers (e.g. CStretchEngine,
// CFX_DIBitmap) take a fresh page fault for every page on every reuse. Keep a
// few recently freed large blocks of the general partition for reuse. PDFium
// is not thread-safe, so neither is this cache.
class LargeBlockCache {
 public:
  static constexpr size_t kMinBlockSize = 1024 * 1024;
  static constexpr size_t kMaxBlockSize = 16 * 1024 * 1024;
  static constexpr size_t kMaxBlocks = 16;
  static constexpr size_t kMaxBytes = 64 * 1024 * 1024;

  // Returns the smallest cached block that holds `size` bytes, or nullptr.
  void* Take(size_t size) {
    if (size < kMinBlockSize || size > kMaxBlockSize) {
      return nullptr;
    }
    size_t best = count_;
    for (size_t i = 0; i < count_; ++i) {
      if (blocks_[i].capacity >= size &&
          (best == count_ || blocks_[i].capacity < blocks_[best].capacity)) {
        best = i;
      }
    }
    if (best == count_) {
      return nullptr;
    }
    void* ptr = blocks_[best].ptr;
    total_bytes_ -= blocks_[best].capacity;
    Remove(best);
    return ptr;
  }

  // Takes ownership of `ptr` if it is a large block; returns false otherwise.
  bool Put(void* ptr) {
    size_t capacity =
        partition_alloc::PartitionRoot::GetExternalUsableSize(ptr);
    if (capacity < kMinBlockSize || capacity > kMaxBlockSize) {
      return false;
    }
    while (count_ == kMaxBlocks || total_bytes_ + capacity > kMaxBytes) {
      total_bytes_ -= blocks_[0].capacity;
      GetGeneralPartitionAllocator().root()->Free(blocks_[0].ptr);
      Remove(0);
    }
    blocks_[count_++] = {ptr, capacity};
    total_bytes_ += capacity;
    return true;
  }

 private:
  struct Block {
    void* ptr;
    size_t capacity;
  };

  void Remove(size_t index) {
    for (size_t i = index + 1; i < count_; ++i) {
      blocks_[i - 1] = blocks_[i];
    }
    --count_;
  }

  std::array<Block, kMaxBlocks> blocks_ = {};
  size_t count_ = 0;
  size_t total_bytes_ = 0;
};

LargeBlockCache& GetLargeBlockCache() {
  static auto* cache = new LargeBlockCache();
  return *cache;
}

}  // namespace

namespace pdfium::internal {

void* Alloc(size_t num_members, size_t member_size) {
  FX_SAFE_SIZE_T total = member_size;
  total *= num_members;
  if (!total.IsValid()) {
    return nullptr;
  }

  void* cached = GetLargeBlockCache().Take(total.ValueOrDie());
  if (cached) {
    return cached;
  }
  return GetGeneralPartitionAllocator()
      .root()
      ->Alloc<partition_alloc::AllocFlags::kReturnNull>(total.ValueOrDie(),
                                                        "GeneralPartition");
}

void* Calloc(size_t num_members, size_t member_size) {
  FX_SAFE_SIZE_T total = member_size;
  total *= num_members;
  if (!total.IsValid()) {
    return nullptr;
  }

  void* cached = GetLargeBlockCache().Take(total.ValueOrDie());
  if (cached) {
    UNSAFE_BUFFERS(FXSYS_memset(cached, 0, total.ValueOrDie()));
    return cached;
  }
  return GetGeneralPartitionAllocator()
      .root()
      ->Alloc<partition_alloc::AllocFlags::kReturnNull |
              partition_alloc::AllocFlags::kZeroFill>(total.ValueOrDie(),
                                                      "GeneralPartition");
}

void* Realloc(void* ptr, size_t num_members, size_t member_size) {
  FX_SAFE_SIZE_T size = num_members;
  size *= member_size;
  if (!size.IsValid()) {
    return nullptr;
  }

  return GetGeneralPartitionAllocator()
      .root()
      ->Realloc<partition_alloc::AllocFlags::kReturnNull>(
          ptr, size.ValueOrDie(), "GeneralPartition");
}

void Dealloc(void* ptr) {
  // TODO(palmer): Removing this check exposes crashes when PDFium callers
  // attempt to free |nullptr|. Although libc's |free| allows freeing |NULL|, no
  // other Partition Alloc callers need this tolerant behavior. Additionally,
  // checking for |nullptr| adds a branch to |PartitionFree|, and it's nice to
  // not have to have that.
  //
  // So this check is hiding (what I consider to be) bugs, and we should try to
  // fix them. https://bugs.chromium.org/p/pdfium/issues/detail?id=690
  if (ptr && !GetLargeBlockCache().Put(ptr)) {
    GetGeneralPartitionAllocator().root()->Free(ptr);
  }
}

void* StringAlloc(size_t num_members, size_t member_size) {
  FX_SAFE_SIZE_T total = member_size;
  total *= num_members;
  if (!total.IsValid()) {
    return nullptr;
  }

  return GetStringPartitionAllocator()
      .root()
      ->Alloc<partition_alloc::AllocFlags::kReturnNull>(total.ValueOrDie(),
                                                        "StringPartition");
}

void StringDealloc(void* ptr) {
  GetStringPartitionAllocator().root()->Free(ptr);
}

}  // namespace pdfium::internal
