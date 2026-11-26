#pragma once

#include "../lockfree/detail/cache_line.h"
#include "lru_cache.h"

#include <chrono>
#include <climits>
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace myframe {
namespace containers {

// Thread-safe monotonic-clock token bucket rate limiter.
//
// Common ads/search uses:
//   - Campaign budget pacing and smooth delivery throttling
//   - Global / per-endpoint QPS protection and burst absorption
//
// Uses `Clock` (default `std::chrono::steady_clock`). Inject a deterministic
// test clock to verify exact refill/burst semantics without sleep().
template <typename Clock = std::chrono::steady_clock>
class TokenBucket {
public:
    typedef Clock clock_type;
    typedef typename Clock::time_point time_point;

    TokenBucket(double rate_per_sec, double burst_capacity)
        : TokenBucket(rate_per_sec, burst_capacity, burst_capacity) {}

    TokenBucket(double rate_per_sec, double burst_capacity, double initial_tokens)
        : mu_(),
          rate_per_sec_(validate_positive(rate_per_sec, "rate_per_sec")),
          burst_capacity_(validate_positive(burst_capacity, "burst_capacity")),
          tokens_(clamp_tokens(initial_tokens, burst_capacity_)),
          last_refill_(Clock::now()) {}

    TokenBucket(const TokenBucket&) = delete;
    TokenBucket& operator=(const TokenBucket&) = delete;

    // Attempts to acquire `permits` tokens immediately without blocking.
    // Returns true if granted, false if insufficient tokens, non-finite/negative
    // `permits`, or `permits > burst_capacity_` (which can never succeed unless
    // update_rate raises burst_capacity_). Note: `try_acquire(0.0)` succeeds.
    bool try_acquire(double permits = 1.0) {
        if (!std::isfinite(permits) || permits < 0.0) {
            return false;
        }
        std::lock_guard<std::mutex> lk(mu_);
        if (permits > burst_capacity_) {
            return false;
        }
        refill_locked(Clock::now());
        const double eps = permits * 1e-12;
        if (tokens_ + eps < permits) {
            return false;
        }
        tokens_ -= permits;
        if (tokens_ < 0.0) {
            tokens_ = 0.0;
        }
        return true;
    }

    double available_tokens() const {
        std::lock_guard<std::mutex> lk(mu_);
        refill_locked(Clock::now());
        return tokens_;
    }

    // Dynamically updates rate and burst capacity (e.g. live pacing adjustment).
    void update_rate(double rate_per_sec, double burst_capacity) {
        const double r = validate_positive(rate_per_sec, "rate_per_sec");
        const double b = validate_positive(burst_capacity, "burst_capacity");
        std::lock_guard<std::mutex> lk(mu_);
        refill_locked(Clock::now());
        rate_per_sec_ = r;
        burst_capacity_ = b;
        if (tokens_ > burst_capacity_) {
            tokens_ = burst_capacity_;
        }
    }

    void reset(double tokens) {
        std::lock_guard<std::mutex> lk(mu_);
        tokens_ = clamp_tokens(tokens, burst_capacity_);
        last_refill_ = Clock::now();
    }

private:
    static double validate_positive(double v, const char* name) {
        if (!std::isfinite(v) || v <= 0.0) {
            throw std::invalid_argument(std::string("TokenBucket ") + name + " must be > 0");
        }
        return v;
    }

    static double clamp_tokens(double v, double cap) noexcept {
        if (!std::isfinite(v) || v < 0.0) {
            return 0.0;
        }
        return (v > cap) ? cap : v;
    }

    void refill_locked(const time_point& now) const noexcept {
        if (now <= last_refill_) {
            return;
        }
        const double elapsed_sec =
            std::chrono::duration<double>(now - last_refill_).count();
        tokens_ += elapsed_sec * rate_per_sec_;
        if (tokens_ > burst_capacity_) {
            tokens_ = burst_capacity_;
        }
        last_refill_ = now;
    }

    mutable std::mutex mu_;
    double rate_per_sec_;
    double burst_capacity_;
    mutable double tokens_;
    mutable time_point last_refill_;
};

// Bounded-memory sharded per-key token-bucket rate limiter.
//
// Common ads/search uses:
//   - Per-IP / per-user / per-campaign / per-tenant QPS rate limiting
//   - Backed by sharded LruCache so high-cardinality keys never cause OOM DoS
template <typename Key,
          typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>,
          typename Clock = std::chrono::steady_clock>
class ShardedRateLimiter {
public:
    typedef Key key_type;
    typedef Clock clock_type;
    typedef typename Clock::time_point time_point;

    ShardedRateLimiter(double rate_per_sec,
                       double burst_capacity,
                       std::size_t max_keys = 65536,
                       std::size_t requested_shards = 64,
                       const Hash& hash = Hash(),
                       const KeyEqual& eq = KeyEqual())
        : rate_per_sec_(validate_positive(rate_per_sec, "rate_per_sec")),
          burst_capacity_(validate_positive(burst_capacity, "burst_capacity")),
          max_keys_(validate_capacity(max_keys)),
          shard_count_(compute_shard_count(max_keys_, requested_shards)),
          shard_mask_(shard_count_ - 1),
          hasher_(hash),
          shards_() {
        shards_.reserve(shard_count_);
        const std::size_t base = max_keys_ / shard_count_;
        const std::size_t rem = max_keys_ % shard_count_;
        for (std::size_t i = 0; i < shard_count_; ++i) {
            const std::size_t cap = base + ((i < rem) ? 1 : 0);
            shards_.emplace_back(new Shard(cap, hash, eq));
        }
    }

    ShardedRateLimiter(const ShardedRateLimiter&) = delete;
    ShardedRateLimiter& operator=(const ShardedRateLimiter&) = delete;

    // Attempts to acquire `permits` for `key`.
    // Eviction hardening: when a shard's LRU is already at capacity (e.g. under
    // high-cardinality / spoofed-IP traffic), newly admitted or evicted keys do
    // NOT receive a full `burst_capacity_` reset; they are capped to at most 1.0
    // initial permit and stored at 0 tokens afterward, preventing burst-bypass
    // via forced LRU eviction.
    bool try_acquire(const Key& key, double permits = 1.0) {
        if (!std::isfinite(permits) || permits < 0.0 || permits > burst_capacity_) {
            return false;
        }
        const double eps = permits * 1e-12;
        const time_point now = Clock::now();
        Shard& shard = shard_for(key);
        std::lock_guard<std::mutex> lk(shard.mu);

        BucketState* st = shard.cache.get_ptr(key);
        if (st == nullptr) {
            const bool under_eviction_pressure =
                (shard.cache.size() >= shard.cache.capacity());
            const double initial_budget =
                under_eviction_pressure
                    ? ((burst_capacity_ < 1.0) ? burst_capacity_ : 1.0)
                    : burst_capacity_;
            if (initial_budget + eps < permits) {
                return false;
            }
            double remaining =
                under_eviction_pressure ? 0.0 : (initial_budget - permits);
            if (remaining < 0.0) {
                remaining = 0.0;
            }
            BucketState fresh = {remaining, now};
            shard.cache.put(key, fresh);
            return true;
        }

        if (now > st->last_refill) {
            const double elapsed_sec =
                std::chrono::duration<double>(now - st->last_refill).count();
            st->tokens += elapsed_sec * rate_per_sec_;
            if (st->tokens > burst_capacity_) {
                st->tokens = burst_capacity_;
            }
            st->last_refill = now;
        }
        if (st->tokens + eps < permits) {
            return false;
        }
        st->tokens -= permits;
        if (st->tokens < 0.0) {
            st->tokens = 0.0;
        }
        return true;
    }

    void clear() {
        for (std::size_t i = 0; i < shard_count_; ++i) {
            std::lock_guard<std::mutex> lk(shards_[i]->mu);
            shards_[i]->cache.clear();
        }
    }

    std::size_t tracked_keys() const {
        std::size_t total = 0;
        for (std::size_t i = 0; i < shard_count_; ++i) {
            std::lock_guard<std::mutex> lk(shards_[i]->mu);
            total += shards_[i]->cache.size();
        }
        return total;
    }

    std::size_t max_keys() const noexcept { return max_keys_; }
    std::size_t shard_count() const noexcept { return shard_count_; }

private:
    static const std::size_t kShardPad = MYFRAME_CACHE_LINE_SIZE;

    struct BucketState {
        double tokens;
        time_point last_refill;
    };

    struct Shard {
        char pad_before[kShardPad];
        mutable std::mutex mu;
        LruCache<Key, BucketState, Hash, KeyEqual> cache;
        char pad_after[kShardPad];

        Shard(std::size_t cap, const Hash& hash, const KeyEqual& eq)
            : pad_before(), mu(), cache(cap, hash, eq), pad_after() {}
    };

    static double validate_positive(double v, const char* name) {
        if (!std::isfinite(v) || v <= 0.0) {
            throw std::invalid_argument(
                std::string("ShardedRateLimiter ") + name + " must be > 0");
        }
        return v;
    }

    static std::size_t validate_capacity(std::size_t cap) {
        if (cap == 0) {
            throw std::invalid_argument("ShardedRateLimiter max_keys must be > 0");
        }
        return cap;
    }

    static std::size_t compute_shard_count(std::size_t max_keys,
                                           std::size_t requested_shards) {
        if (requested_shards == 0) {
            throw std::invalid_argument("ShardedRateLimiter shard count must be > 0");
        }
        static const std::size_t kMaxShards = 65536;
        if (requested_shards > kMaxShards) {
            throw std::invalid_argument(
                "ShardedRateLimiter shard count exceeds maximum (65536)");
        }
        std::size_t n = 1;
        while (n < requested_shards) {
            n <<= 1;
        }
        while (n > max_keys && n > 1) {
            n >>= 1;
        }
        return n;
    }

    std::size_t shard_index(const Key& key) const {
        std::size_t h = hasher_(key);
        const std::size_t half_bits = (sizeof(std::size_t) * CHAR_BIT) / 2;
        h ^= (h >> half_bits);
        h *= static_cast<std::size_t>(0x9e3779b97f4a7c15ULL);
        h ^= (h >> half_bits);
        return h & shard_mask_;
    }

    Shard& shard_for(const Key& key) {
        return *shards_[shard_index(key)];
    }

    double rate_per_sec_;
    double burst_capacity_;
    std::size_t max_keys_;
    std::size_t shard_count_;
    std::size_t shard_mask_;
    Hash hasher_;
    std::vector<std::unique_ptr<Shard> > shards_;
};

} // namespace containers
} // namespace myframe
