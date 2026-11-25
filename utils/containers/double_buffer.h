#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace myframe {
namespace containers {

#ifndef MYFRAME_CACHE_LINE_SIZE
#define MYFRAME_CACHE_LINE_SIZE 128
#endif

// Lock-free-read double-buffered snapshot container for hot-reloading read-mostly
// data structures in ads and search servers.
//
// Common ads/search uses:
//   - Zero-stall background hot-reload of AhoCorasick dictionaries, RadixTree
//     routing tables, InvertedIndex targeting snapshots, BloomFilters,
//     ConsistentHash rings, and scoring configs.
//
// Concurrency design (strict C++11, ARM64 weak-ordering & TSan clean):
//   - Readers take NO mutex and contend only on their own cache-line-padded
//     reader stripe (64 stripes, assigned via a trivial POD thread_local index —
//     zero thread-exit destructors and zero static-destruction-order hazards).
//   - Writer publishes a new snapshot under `writer_mu_`, flips `active_index_`
//     with `memory_order_seq_cst`, waits for active readers on the previous slot
//     to drain (paired `seq_cst` Dekker handshake — no standalone fences), then
//     destroys the retired snapshot.
//
// Preconditions:
//   - A thread must NOT call publish()/update_copy() while holding a ReadGuard
//     on the same DoubleBuffer instance (would wait for its own guard).
//   - Destruction (~DoubleBuffer) must occur after all readers and writers exit.
template <typename T>
class DoubleBuffer {
private:
    static const std::size_t kPad = MYFRAME_CACHE_LINE_SIZE;
    static const std::size_t kStripes = 64;

    struct ReaderStripe {
        char pad_before[kPad];
        mutable std::atomic<std::size_t> readers[2];
        char pad_after[kPad];

        ReaderStripe() noexcept : pad_before(), pad_after() {
            readers[0].store(0, std::memory_order_relaxed);
            readers[1].store(0, std::memory_order_relaxed);
        }
    };

public:
    typedef T value_type;

    class ReadGuard {
    public:
        ReadGuard() noexcept : stripe_(nullptr), slot_(0), ptr_(nullptr) {}

        ~ReadGuard() noexcept {
            release();
        }

        ReadGuard(const ReadGuard&) = delete;
        ReadGuard& operator=(const ReadGuard&) = delete;

        ReadGuard(ReadGuard&& other) noexcept
            : stripe_(other.stripe_), slot_(other.slot_), ptr_(other.ptr_) {
            other.stripe_ = nullptr;
            other.ptr_ = nullptr;
        }

        ReadGuard& operator=(ReadGuard&& other) noexcept {
            if (this != &other) {
                release();
                stripe_ = other.stripe_;
                slot_ = other.slot_;
                ptr_ = other.ptr_;
                other.stripe_ = nullptr;
                other.ptr_ = nullptr;
            }
            return *this;
        }

        const T* get() const noexcept { return ptr_; }
        const T& operator*() const noexcept { return *ptr_; }
        const T* operator->() const noexcept { return ptr_; }
        explicit operator bool() const noexcept { return ptr_ != nullptr; }

    private:
        friend class DoubleBuffer<T>;

        ReadGuard(const ReaderStripe* stripe, int slot, const T* ptr) noexcept
            : stripe_(stripe), slot_(slot), ptr_(ptr) {}

        void release() noexcept {
            if (stripe_ != nullptr) {
                stripe_->readers[slot_].fetch_sub(1, std::memory_order_release);
                stripe_ = nullptr;
                ptr_ = nullptr;
            }
        }

        const ReaderStripe* stripe_;
        int slot_;
        const T* ptr_;
    };

    DoubleBuffer()
        : DoubleBuffer(std::unique_ptr<T>(new T())) {}

    explicit DoubleBuffer(std::unique_ptr<T> initial)
        : pad_front_(),
          active_index_(0),
          version_(1),
          pad_mid_(),
          writer_mu_(),
          stripes_(new ReaderStripe[kStripes]) {
        if (!initial) {
            throw std::invalid_argument("DoubleBuffer initial pointer must be non-null");
        }
        slots_[0] = std::move(initial);
    }

    explicit DoubleBuffer(const T& initial)
        : DoubleBuffer(std::unique_ptr<T>(new T(initial))) {}

    explicit DoubleBuffer(T&& initial)
        : DoubleBuffer(std::unique_ptr<T>(new T(std::move(initial)))) {}

    DoubleBuffer(const DoubleBuffer&) = delete;
    DoubleBuffer& operator=(const DoubleBuffer&) = delete;

    // Acquires a lock-free read guard to the current active snapshot.
    // Nested read() calls on the same thread are supported.
    ReadGuard read() const noexcept {
        const ReaderStripe& stripe = stripes_[thread_stripe_index()];
        for (;;) {
            const int idx = active_index_.load(std::memory_order_acquire);
            stripe.readers[idx].fetch_add(1, std::memory_order_seq_cst);
            if (active_index_.load(std::memory_order_seq_cst) == idx) {
                return ReadGuard(&stripe, idx, slots_[idx].get());
            }
            stripe.readers[idx].fetch_sub(1, std::memory_order_release);
        }
    }

    // Atomically publishes a freshly built snapshot and waits for readers of the
    // retired snapshot to finish before destroying the old object.
    void publish(std::unique_ptr<T> next) {
        if (!next) {
            throw std::invalid_argument("DoubleBuffer::publish pointer must be non-null");
        }
        std::lock_guard<std::mutex> lk(writer_mu_);
        publish_locked(std::move(next));
    }

    void publish(const T& next) {
        publish(std::unique_ptr<T>(new T(next)));
    }

    void publish(T&& next) {
        publish(std::unique_ptr<T>(new T(std::move(next))));
    }

    // Copy-on-write mutation helper: clones the active snapshot, invokes `fn(T&)`
    // on the clone, and atomically publishes it. Strong exception guarantee.
    template <typename Fn>
    void update_copy(Fn&& fn) {
        std::lock_guard<std::mutex> lk(writer_mu_);
        const int cur = active_index_.load(std::memory_order_relaxed);
        std::unique_ptr<T> copy(new T(*slots_[cur]));
        fn(*copy);
        publish_locked(std::move(copy));
    }

    std::uint64_t version() const noexcept {
        return version_.load(std::memory_order_acquire);
    }

private:
    static std::size_t thread_stripe_index() noexcept {
        static std::atomic<std::size_t> next_id(0);
        static thread_local std::size_t my_stripe =
            next_id.fetch_add(1, std::memory_order_relaxed) & (kStripes - 1);
        return my_stripe;
    }

    void publish_locked(std::unique_ptr<T> next) noexcept {
        const int cur = active_index_.load(std::memory_order_relaxed);
        const int bg = 1 - cur;
        slots_[bg] = std::move(next);
        active_index_.store(bg, std::memory_order_seq_cst);
        version_.fetch_add(1, std::memory_order_release);
        wait_for_readers_to_drain(cur);
        slots_[cur].reset();
    }

    void wait_for_readers_to_drain(int slot) const noexcept {
        for (std::size_t s = 0; s < kStripes; ++s) {
            int spins = 0;
            while (stripes_[s].readers[slot].load(std::memory_order_seq_cst) != 0) {
                if (++spins >= 64) {
                    std::this_thread::yield();
                    spins = 0;
                }
            }
        }
    }

    char pad_front_[kPad];
    std::atomic<int> active_index_;
    std::atomic<std::uint64_t> version_;
    char pad_mid_[kPad];
    std::mutex writer_mu_;
    std::unique_ptr<T> slots_[2];
    std::unique_ptr<ReaderStripe[]> stripes_;
};

} // namespace containers
} // namespace myframe
