#pragma once

#include <cstddef>

namespace myframe {
namespace lockfree {
namespace detail {

#ifndef MYFRAME_CACHE_LINE_SIZE
// 128 bytes avoids false sharing on both 64-byte x86 lines with Intel's
// adjacent-line spatial prefetcher and 128-byte cache lines on ARM64 cores.
#define MYFRAME_CACHE_LINE_SIZE 128
#endif

static const std::size_t kFalseSharingRange = MYFRAME_CACHE_LINE_SIZE;
static_assert((kFalseSharingRange >= 64) &&
                  ((kFalseSharingRange & (kFalseSharingRange - 1)) == 0),
              "MYFRAME_CACHE_LINE_SIZE must be a power of two >= 64");

} // namespace detail
} // namespace lockfree
} // namespace myframe
