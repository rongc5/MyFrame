#pragma once

#include "detail/aligned_buffer.h"
#include "detail/cache_line.h"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace myframe {
namespace lockfree {

// Bounded wait-free Single-Producer Single-Consumer (SPSC) ring queue.
//
// Thread-safety contract:
//   - At most ONE thread may call producer methods (try_push, try_emplace, try_push_n).
//   - At most ONE thread may call consumer methods (try_pop, try_consume, front, pop, try_pop_n).
//   - Destruction (~SpscQueue) must not run concurrently with any operation.
//
// Properties:
//   - Strict C++11, zero dynamic allocation after construction, no C++17 aligned new.
//   - Exact requested capacity (backed by capacity + 1 raw storage slots).
//   - Supports move-only and non-default-constructible types.
//   - Strong exception guarantee on try_push / try_emplace / try_push_n.
//   - Cached remote indices eliminate cross-core cache-line traffic in steady state.
template <typename T>
class SpscQueue {
    static_assert(std::is_nothrow_destructible<T>::value,
                  "SpscQueue<T> requires nothrow-destructible T");

public:
    typedef T value_type;

    explicit SpscQueue(std::size_t capacity)
        : pad_front_(),
          buffer_(),
          capacity_(validate_capacity(capacity)),
          slots_(capacity_ + 1),
          pad_prod_(),
          tail_(0),
          head_cache_(0),
          pad_cons_(),
          head_(0),
          tail_cache_(0),
          pad_end_() {
        buffer_.allocate(slots_, detail::kFalseSharingRange);
        Slot* raw_slots = buffer_.data();
        for (std::size_t i = 0; i < slots_; ++i) {
            ::new (static_cast<void*>(&raw_slots[i])) Slot();
        }
    }

    ~SpscQueue() noexcept {
        if (buffer_.data() == nullptr) {
            return;
        }
        std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        while (h != t) {
            slot(h)->ptr()->~T();
            h = increment(h);
        }
    }

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    // -------------------------------------------------------------------------
    // Producer API (single producer thread only)
    // -------------------------------------------------------------------------

    // Constructs an element in-place. Returns false without evaluating or
    // moving from arguments if the queue is full. Strong exception guarantee.
    template <typename... Args>
    bool try_emplace(Args&&... args) {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        const std::size_t next = increment(t);
        if (next == head_cache_) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (next == head_cache_) {
                return false;
            }
        }
        ::new (static_cast<void*>(slot(t)->storage)) T(std::forward<Args>(args)...);
        tail_.store(next, std::memory_order_release);
        return true;
    }

    bool try_push(const T& value) {
        return try_emplace(value);
    }

    // On failure (returns false), `value` is guaranteed NOT to have been moved from.
    bool try_push(T&& value) {
        return try_emplace(std::move(value));
    }

    // Batch push up to `count` elements from `first` with a single release store.
    // Pass std::make_move_iterator(ptr) to move elements (including move-only T).
    // Strong queue exception guarantee: if construction throws mid-batch, already
    // constructed queue slots in this batch are rolled back and tail_ is unchanged
    // (note: moved-from elements in a move_iterator input range are not restored).
    template <typename InputIt>
    std::size_t try_push_n(InputIt first, std::size_t count) {
        if (count == 0) {
            return 0;
        }
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        std::size_t avail = free_slots(t, head_cache_);
        if (avail < count) {
            head_cache_ = head_.load(std::memory_order_acquire);
            avail = free_slots(t, head_cache_);
        }
        const std::size_t n = (count < avail) ? count : avail;
        if (n == 0) {
            return 0;
        }

        std::size_t cur = t;
        std::size_t constructed = 0;
        try {
            for (; constructed < n; ++constructed, ++first) {
                ::new (static_cast<void*>(slot(cur)->storage)) T(*first);
                cur = increment(cur);
            }
        } catch (...) {
            std::size_t rollback = t;
            for (std::size_t i = 0; i < constructed; ++i) {
                slot(rollback)->ptr()->~T();
                rollback = increment(rollback);
            }
            throw;
        }

        tail_.store(cur, std::memory_order_release);
        return n;
    }

    // -------------------------------------------------------------------------
    // Consumer API (single consumer thread only)
    // -------------------------------------------------------------------------

    // Pops one element into `out`. If move-assignment throws, the (possibly
    // partially moved-from) element remains queued (head_ is not advanced) so
    // no leak or double-destroy occurs. (Contrast with MpmcQueue::try_pop, which
    // destroys and drops the element on throw to avoid wedging the slot.)
    bool try_pop(T& out) {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (h == tail_cache_) {
                return false;
            }
        }
        T* p = slot(h)->ptr();
        out = std::move(*p);
        p->~T();
        head_.store(increment(h), std::memory_order_release);
        return true;
    }

    // Zero-copy callback consumption. Passes T&& to `fn`.
    // Basic exception guarantee: even if `fn` throws, the popped element is
    // destroyed and head_ advances so the queue stays healthy.
    // Precondition: `fn` must NOT re-entrantly call consumer methods on this queue.
    template <typename Fn>
    bool try_consume(Fn&& fn) {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (h == tail_cache_) {
                return false;
            }
        }
        T* p = slot(h)->ptr();
        const std::size_t next = increment(h);
        struct ConsumeGuard {
            T* ptr;
            std::atomic<std::size_t>* head;
            std::size_t next_idx;
            ~ConsumeGuard() noexcept {
                ptr->~T();
                head->store(next_idx, std::memory_order_release);
            }
        } guard = {p, &head_, next};
        (void)guard;
        fn(std::move(*p));
        return true;
    }

    // Zero-copy peek at the front element (consumer thread only).
    // Returns nullptr if empty. Must be paired with pop() once consumed.
    T* front() noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (h == tail_cache_) {
                return nullptr;
            }
        }
        return slot(h)->ptr();
    }

    // Destroys and releases the front element previously observed via front().
    // Precondition: front() returned non-null.
    void pop() noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        assert(h != tail_cache_);
        slot(h)->ptr()->~T();
        head_.store(increment(h), std::memory_order_release);
    }

    // Batch pop up to `max_count` elements into `out` with a single release store.
    // If assignment to `*out` throws mid-batch, elements already popped before the
    // throw are published to head_ so they are never double-destroyed, and the
    // throwing element stays queued.
    template <typename OutputIt>
    std::size_t try_pop_n(OutputIt out, std::size_t max_count) {
        if (max_count == 0) {
            return 0;
        }
        const std::size_t h = head_.load(std::memory_order_relaxed);
        std::size_t avail = used_slots(h, tail_cache_);
        if (avail < max_count) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            avail = used_slots(h, tail_cache_);
        }
        const std::size_t n = (max_count < avail) ? max_count : avail;
        if (n == 0) {
            return 0;
        }

        std::size_t cur = h;
        std::size_t popped = 0;
        try {
            for (; popped < n; ++popped) {
                T* p = slot(cur)->ptr();
                *out = std::move(*p);
                p->~T();
                cur = increment(cur);
                ++out;
            }
        } catch (...) {
            if (cur != h) {
                head_.store(cur, std::memory_order_release);
            }
            throw;
        }

        head_.store(cur, std::memory_order_release);
        return n;
    }

    // -------------------------------------------------------------------------
    // Observers (safe from any thread; approximate under concurrency)
    // -------------------------------------------------------------------------

    std::size_t capacity() const noexcept {
        return capacity_;
    }

    std::size_t size_approx() const noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        return used_slots(h, t);
    }

    bool empty_approx() const noexcept {
        return head_.load(std::memory_order_relaxed) ==
               tail_.load(std::memory_order_relaxed);
    }

private:
    struct Slot {
        alignas(T) unsigned char storage[sizeof(T)];

        T* ptr() noexcept {
#if __cplusplus >= 201703L
            return std::launder(reinterpret_cast<T*>(storage));
#else
            return reinterpret_cast<T*>(storage);
#endif
        }
    };

    static std::size_t validate_capacity(std::size_t cap) {
        if (cap == 0) {
            throw std::invalid_argument("SpscQueue capacity must be > 0");
        }
        if (cap >= std::numeric_limits<std::size_t>::max()) {
            throw std::invalid_argument("SpscQueue capacity overflow");
        }
        const std::size_t total_slots = cap + 1;
        const std::size_t alignment =
            (alignof(Slot) > detail::kFalseSharingRange)
                ? alignof(Slot)
                : detail::kFalseSharingRange;
        if (total_slots >
            (std::numeric_limits<std::size_t>::max() - (alignment - 1)) / sizeof(Slot)) {
            throw std::invalid_argument("SpscQueue capacity overflow");
        }
        return cap;
    }

    Slot* slot(std::size_t index) noexcept {
        return &buffer_.data()[index];
    }

    std::size_t increment(std::size_t index) const noexcept {
        const std::size_t next = index + 1;
        return (next == slots_) ? 0 : next;
    }

    std::size_t free_slots(std::size_t t, std::size_t h) const noexcept {
        return (h > t) ? (h - t - 1) : (slots_ - t + h - 1);
    }

    std::size_t used_slots(std::size_t h, std::size_t t) const noexcept {
        return (t >= h) ? (t - h) : (slots_ - h + t);
    }

    // Leading guard against false sharing with preceding objects.
    char pad_front_[detail::kFalseSharingRange];

    // Immutable metadata after construction.
    detail::AlignedBuffer<Slot> buffer_;
    std::size_t capacity_;
    std::size_t slots_;

    // Producer-only hot state, separated by >= kFalseSharingRange bytes.
    char pad_prod_[detail::kFalseSharingRange];
    std::atomic<std::size_t> tail_;
    std::size_t head_cache_;

    // Consumer-only hot state, separated by >= kFalseSharingRange bytes.
    char pad_cons_[detail::kFalseSharingRange];
    std::atomic<std::size_t> head_;
    std::size_t tail_cache_;

    // Trailing guard against false sharing with subsequent objects.
    char pad_end_[detail::kFalseSharingRange];
};

} // namespace lockfree
} // namespace myframe
