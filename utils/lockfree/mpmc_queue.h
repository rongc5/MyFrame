#pragma once

#include "detail/aligned_buffer.h"
#include "detail/cache_line.h"

#include <atomic>
#include <climits>
#include <cstddef>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace myframe {
namespace lockfree {

namespace detail {
struct MpmcQueueTestAccess;
} // namespace detail

// Bounded Multi-Producer Multi-Consumer (MPMC) ring queue using Dmitry Vyukov's
// per-slot sequence-number algorithm.
//
// Thread-safety contract:
//   - Any number of producer and consumer threads may call try_push, try_emplace,
//     try_pop, try_consume, size_approx, empty_approx, and capacity concurrently.
//   - Also works efficiently for MPSC (consumer CAS is uncontended).
//   - Destruction (~MpmcQueue) must not run concurrently with any operation.
//
// Progress guarantee (important):
//   - Mutex-free / CAS-based: individual try_* calls never block on an OS mutex
//     and return false immediately when the head/tail slot is not ready.
//   - NOT formally lock-free in the strict theoretical progress sense: a producer
//     preempted after reserving `enqueue_pos_` and before publishing `cell->seq`
//     stalls logical dequeue progress at that reserved head slot (consumers see
//     `seq != pos + 1` and return false, and producers eventually see a full ring
//     on wrap-around) until that producer finishes publishing. Symmetrically, a
//     consumer preempted between reserving `dequeue_pos_` and releasing `cell->seq`
//     prevents reuse of that slot on wrap-around until it completes.
//
// Properties:
//   - Strict C++11, zero dynamic allocation after construction, no ABA problem,
//     no hazard pointers / epoch reclamation required, no C++17 aligned new.
//   - Capacity is rounded up to a power of two with minimum 2; capacity() returns
//     the actual usable capacity.
//   - Exception-safe: T must be nothrow-move-constructible and nothrow-destructible
//     (wrap throwing-move types in std::unique_ptr<T>). Throwing constructors in
//     try_push(const T&) or try_emplace(...) run BEFORE claiming a slot, so a
//     throwing constructor can never wedge the ring. Throwing consumer callbacks
//     or move-assignment still destroy and release the slot via RAII scope guard.
//   - Contract on failure: when try_push(T&&) returns false, the rvalue argument
//     has NOT been moved from. On the throwing-construction path of try_emplace,
//     rvalue arguments may be consumed if the queue becomes full concurrently.
template <typename T>
class MpmcQueue {
    static_assert(std::is_nothrow_move_constructible<T>::value,
                  "MpmcQueue<T> requires nothrow-move-constructible T "
                  "(wrap throwing-move types in std::unique_ptr<T>)");
    static_assert(std::is_nothrow_destructible<T>::value,
                  "MpmcQueue<T> requires nothrow-destructible T");

public:
    typedef T value_type;

    explicit MpmcQueue(std::size_t requested_capacity)
        : MpmcQueue(requested_capacity, 0) {}

    ~MpmcQueue() noexcept {
        Cell* const cells = buffer_.data();
        if (cells == nullptr) {
            return;
        }
        const std::size_t deq = dequeue_pos_.load(std::memory_order_relaxed);
        const std::size_t enq = enqueue_pos_.load(std::memory_order_relaxed);
        for (std::size_t pos = deq; pos != enq; ++pos) {
            Cell* cell = &cells[pos & mask_];
            if (cell->seq.load(std::memory_order_relaxed) == pos + 1) {
                cell->ptr()->~T();
            }
        }
        for (std::size_t i = 0; i < capacity_; ++i) {
            cells[i].~Cell();
        }
    }

    MpmcQueue(const MpmcQueue&) = delete;
    MpmcQueue& operator=(const MpmcQueue&) = delete;

    // -------------------------------------------------------------------------
    // Producer API (thread-safe for multiple concurrent producers)
    // -------------------------------------------------------------------------

    template <typename... Args>
    bool try_emplace(Args&&... args) {
        return try_emplace_impl(
            std::integral_constant<
                bool,
                std::is_nothrow_constructible<T, Args&&...>::value>(),
            std::forward<Args>(args)...);
    }

    bool try_push(const T& value) {
        return try_emplace(value);
    }

    // Because T is nothrow-move-constructible, try_push(T&&) always takes the
    // direct post-claim path: if the queue is full (returns false), `value` is
    // guaranteed NOT to have been moved from.
    bool try_push(T&& value) noexcept {
        return try_emplace_impl(std::true_type(), std::move(value));
    }

    // -------------------------------------------------------------------------
    // Consumer API (thread-safe for multiple concurrent consumers)
    // -------------------------------------------------------------------------

    // Zero-copy callback consumption passing T&& to `fn`.
    // Basic exception guarantee: even if `fn` throws, the claimed slot's element
    // is destroyed and its sequence number is released so the queue never wedges.
    template <typename Fn>
    bool try_consume(Fn&& fn) {
        Cell* const cells = buffer_.data();
        std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        Cell* cell = nullptr;
        for (;;) {
            cell = &cells[pos & mask_];
            const std::size_t seq = cell->seq.load(std::memory_order_acquire);
            const std::size_t diff = seq - (pos + 1);
            if (diff == 0) {
                if (dequeue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (is_negative_diff(diff)) {
                return false;
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }

        T* p = cell->ptr();
        struct ReleaseGuard {
            T* ptr;
            Cell* slot;
            std::size_t next_seq;
            ~ReleaseGuard() noexcept {
                ptr->~T();
                slot->seq.store(next_seq, std::memory_order_release);
            }
        } guard = {p, cell, pos + mask_ + 1};
        (void)guard;
        fn(std::move(*p));
        return true;
    }

    // Pops one element into `out`. If move-assignment throws, the popped element
    // is still destroyed and the slot released (basic exception guarantee;
    // contrast with SpscQueue::try_pop which keeps the element queued).
    bool try_pop(T& out) {
        return try_consume([&out](T&& value) {
            out = std::move(value);
        });
    }

    // -------------------------------------------------------------------------
    // Observers (safe from any thread; approximate under concurrency)
    // -------------------------------------------------------------------------

    std::size_t capacity() const noexcept {
        return capacity_;
    }

    std::size_t size_approx() const noexcept {
        const std::size_t deq = dequeue_pos_.load(std::memory_order_relaxed);
        const std::size_t enq = enqueue_pos_.load(std::memory_order_relaxed);
        const std::size_t diff = enq - deq;
        if (is_negative_diff(diff)) {
            return 0;
        }
        return (diff > capacity_) ? capacity_ : diff;
    }

    bool empty_approx() const noexcept {
        return size_approx() == 0;
    }

private:
    friend struct detail::MpmcQueueTestAccess;

    struct Cell {
        std::atomic<std::size_t> seq;
        alignas(T) unsigned char storage[sizeof(T)];

        explicit Cell(std::size_t initial_seq) noexcept : seq(initial_seq) {}

        T* ptr() noexcept {
#if __cplusplus >= 201703L
            return std::launder(reinterpret_cast<T*>(storage));
#else
            return reinterpret_cast<T*>(storage);
#endif
        }
    };

    // Internal constructor allowing unit tests to start near SIZE_MAX to
    // exercise modular wrap-around deterministically in milliseconds.
    MpmcQueue(std::size_t requested_capacity, std::size_t initial_pos)
        : pad_front_(),
          buffer_(),
          capacity_(round_up_capacity(requested_capacity)),
          mask_(capacity_ - 1),
          pad_enq_(),
          enqueue_pos_(initial_pos),
          pad_deq_(),
          dequeue_pos_(initial_pos),
          pad_end_() {
        buffer_.allocate(capacity_, detail::kFalseSharingRange);
        Cell* const cells = buffer_.data();
        for (std::size_t offset = 0; offset < capacity_; ++offset) {
            const std::size_t seq = initial_pos + offset;
            ::new (static_cast<void*>(&cells[seq & mask_])) Cell(seq);
        }
    }

    static bool is_negative_diff(std::size_t diff) noexcept {
        static const std::size_t kSignBit =
            std::size_t(1) << (sizeof(std::size_t) * CHAR_BIT - 1);
        return (diff & kSignBit) != 0;
    }

    static std::size_t round_up_capacity(std::size_t requested) {
        if (requested == 0) {
            throw std::invalid_argument("MpmcQueue capacity must be > 0");
        }
        if (requested == 1) {
            return 2;
        }
        const std::size_t alignment =
            (alignof(Cell) > detail::kFalseSharingRange)
                ? alignof(Cell)
                : detail::kFalseSharingRange;
        const std::size_t max_by_alloc =
            (std::numeric_limits<std::size_t>::max() - (alignment - 1)) / sizeof(Cell);
        const std::size_t max_by_wrap =
            std::size_t(1) << (sizeof(std::size_t) * CHAR_BIT - 2);
        const std::size_t max_cap =
            (max_by_alloc < max_by_wrap) ? max_by_alloc : max_by_wrap;

        if (requested > max_cap) {
            throw std::invalid_argument("MpmcQueue capacity overflow");
        }
        std::size_t cap = 2;
        while (cap < requested) {
            if (cap > max_cap / 2) {
                throw std::invalid_argument("MpmcQueue capacity overflow");
            }
            cap <<= 1;
        }
        return cap;
    }

    bool looks_full_hint() const noexcept {
        const std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        const Cell* cell = &buffer_.data()[pos & mask_];
        const std::size_t seq = cell->seq.load(std::memory_order_relaxed);
        return is_negative_diff(seq - pos);
    }

    bool claim_enqueue_slot(Cell*& out_cell, std::size_t& out_pos) noexcept {
        Cell* const cells = buffer_.data();
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            Cell* cell = &cells[pos & mask_];
            const std::size_t seq = cell->seq.load(std::memory_order_acquire);
            const std::size_t diff = seq - pos;
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed)) {
                    out_cell = cell;
                    out_pos = pos;
                    return true;
                }
            } else if (is_negative_diff(diff)) {
                return false;
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
    }

    // Nothrow construction path: claim slot first, construct directly in place.
    template <typename... Args>
    bool try_emplace_impl(std::true_type, Args&&... args) noexcept {
        Cell* cell = nullptr;
        std::size_t pos = 0;
        if (!claim_enqueue_slot(cell, pos)) {
            return false;
        }
        ::new (static_cast<void*>(cell->storage)) T(std::forward<Args>(args)...);
        cell->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    // Potentially-throwing construction path: build temporary BEFORE claiming
    // any slot, so an exception never leaves a claimed slot unpublished.
    template <typename... Args>
    bool try_emplace_impl(std::false_type, Args&&... args) {
        if (looks_full_hint()) {
            return false;
        }
        T tmp(std::forward<Args>(args)...);
        Cell* cell = nullptr;
        std::size_t pos = 0;
        if (!claim_enqueue_slot(cell, pos)) {
            return false;
        }
        ::new (static_cast<void*>(cell->storage)) T(std::move(tmp));
        cell->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    // Leading guard against false sharing with preceding objects.
    char pad_front_[detail::kFalseSharingRange];

    // Immutable metadata after construction.
    detail::AlignedBuffer<Cell> buffer_;
    std::size_t capacity_;
    std::size_t mask_;

    // Producer contention line, padded >= kFalseSharingRange bytes apart.
    char pad_enq_[detail::kFalseSharingRange];
    std::atomic<std::size_t> enqueue_pos_;

    // Consumer contention line, padded >= kFalseSharingRange bytes apart.
    char pad_deq_[detail::kFalseSharingRange];
    std::atomic<std::size_t> dequeue_pos_;

    // Trailing guard against false sharing with subsequent objects.
    char pad_end_[detail::kFalseSharingRange];
};

} // namespace lockfree
} // namespace myframe
