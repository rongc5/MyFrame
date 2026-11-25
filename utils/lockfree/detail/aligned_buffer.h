#pragma once

#include "cache_line.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>

namespace myframe {
namespace lockfree {
namespace detail {

// Pure ISO C++11 over-aligned raw storage RAII owner.
// Uses ::operator new + manual alignment adjustment so it never depends on
// C++17 over-aligned operator new (avoiding -Waligned-new under -std=c++11)
// and never depends on POSIX-only macros when extensions are disabled.
template <typename T>
class AlignedBuffer {
public:
    AlignedBuffer() noexcept : raw_(nullptr), data_(nullptr) {}

    ~AlignedBuffer() noexcept {
        reset();
    }

    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;

    void allocate(std::size_t count, std::size_t min_alignment = kFalseSharingRange) {
        reset();
        if (count == 0) {
            return;
        }
        const std::size_t alignment =
            (alignof(T) > min_alignment) ? alignof(T) : min_alignment;
        assert(alignment > 0 && ((alignment & (alignment - 1)) == 0));

        const std::size_t pad = alignment - 1;
        if (count > (std::numeric_limits<std::size_t>::max() - pad) / sizeof(T)) {
            throw std::bad_alloc();
        }
        const std::size_t total_bytes = count * sizeof(T) + pad;
        void* raw = ::operator new(total_bytes);

        const std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(raw);
        const std::size_t rem = static_cast<std::size_t>(addr & pad);
        const std::size_t offset = (rem == 0) ? 0 : (alignment - rem);

        raw_ = raw;
        data_ = reinterpret_cast<T*>(static_cast<unsigned char*>(raw) + offset);
    }

    void reset() noexcept {
        if (raw_ != nullptr) {
            ::operator delete(raw_);
            raw_ = nullptr;
            data_ = nullptr;
        }
    }

    T* data() noexcept { return data_; }
    const T* data() const noexcept { return data_; }

private:
    void* raw_;
    T* data_;
};

} // namespace detail
} // namespace lockfree
} // namespace myframe
