#pragma once

#include "lru_cache.h"

#include <chrono>
#include <climits>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace myframe {
namespace containers {

#ifndef MYFRAME_CACHE_LINE_SIZE
#define MYFRAME_CACHE_LINE_SIZE 128
#endif

// Thread-safe sharded LRU cache with per-entry monotonic TTL expiration.
//
// Common ads/search uses:
//   - User profile / audience-tag cache with TTL
//   - Query result & recall cache
//   - Creative / campaign metadata cache and frequency-cap windows
//
// Design:
//   - Distributes `total_capacity` across N power-of-two cache-line-padded shards
//     (each holding a mutex + LruCache).
//   - Uses `Clock` (default `std::chrono::steady_clock`, monotonic and immune to
//     wall-clock jumps). A custom test clock can be injected for zero-sleep tests.
//   - Expired entries are lazily evicted on get()/visit() and can be swept
//     periodically via purge_expired().
template <typename Key,
          typename Value,
          typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>,
          typename Clock = std::chrono::steady_clock>
class ShardedTtlCache {
public:
    typedef Key key_type;
    typedef Value mapped_type;
    typedef Clock clock_type;
    typedef typename Clock::time_point time_point;

    explicit ShardedTtlCache(std::size_t total_capacity,
                             std::size_t requested_shards = 64,
                             const Hash& hash = Hash(),
                             const KeyEqual& eq = KeyEqual())
        : total_capacity_(validate_capacity(total_capacity)),
          shard_count_(compute_shard_count(total_capacity_, requested_shards)),
          shard_mask_(shard_count_ - 1),
          hasher_(hash),
          shards_() {
        shards_.reserve(shard_count_);
        const std::size_t base = total_capacity_ / shard_count_;
        const std::size_t rem = total_capacity_ % shard_count_;
        for (std::size_t i = 0; i < shard_count_; ++i) {
            const std::size_t shard_cap = base + ((i < rem) ? 1 : 0);
            shards_.emplace_back(new Shard(shard_cap, hash, eq));
        }
    }

    ShardedTtlCache(const ShardedTtlCache&) = delete;
    ShardedTtlCache& operator=(const ShardedTtlCache&) = delete;

    // Inserts or updates `key` with TTL in milliseconds (`ttl_ms == 0` = no expiry).
    // Returns true if inserted new key, false if overwritten.
    bool put(const Key& key, const Value& value, std::uint64_t ttl_ms = 0) {
        const Entry entry = make_entry(value, ttl_ms);
        Shard& shard = shard_for(key);
        std::lock_guard<std::mutex> lk(shard.mu);
        return shard.cache.put(key, entry);
    }

    bool put(const Key& key, Value&& value, std::uint64_t ttl_ms = 0) {
        Entry entry = make_entry_move(std::move(value), ttl_ms);
        Shard& shard = shard_for(key);
        std::lock_guard<std::mutex> lk(shard.mu);
        return shard.cache.put(key, std::move(entry));
    }

    // Copies the value into `out` if present and unexpired (promoting to MRU).
    // Expired entries are evicted on lookup and treated as a miss.
    bool get(const Key& key, Value& out) {
        Shard& shard = shard_for(key);
        std::lock_guard<std::mutex> lk(shard.mu);
        Entry* entry = shard.cache.get_ptr(key);
        if (entry == nullptr) {
            return false;
        }
        if (is_expired(*entry, Clock::now())) {
            shard.cache.erase(key);
            return false;
        }
        out = entry->value;
        return true;
    }

    // Atomically visits an unexpired entry under the shard lock (`fn(Value&)`).
    template <typename Fn>
    bool visit(const Key& key, Fn&& fn) {
        Shard& shard = shard_for(key);
        std::lock_guard<std::mutex> lk(shard.mu);
        Entry* entry = shard.cache.get_ptr(key);
        if (entry == nullptr) {
            return false;
        }
        if (is_expired(*entry, Clock::now())) {
            shard.cache.erase(key);
            return false;
        }
        fn(entry->value);
        return true;
    }

    bool erase(const Key& key) {
        Shard& shard = shard_for(key);
        std::lock_guard<std::mutex> lk(shard.mu);
        return shard.cache.erase(key);
    }

    // Sweeps all shards one at a time and removes expired entries.
    // Returns total number of purged entries.
    std::size_t purge_expired() {
        const time_point now = Clock::now();
        std::size_t purged = 0;
        for (std::size_t i = 0; i < shard_count_; ++i) {
            std::lock_guard<std::mutex> lk(shards_[i]->mu);
            purged += shards_[i]->cache.erase_if(
                [&now](const Key&, const Entry& e) {
                    return is_expired(e, now);
                });
        }
        return purged;
    }

    void clear() {
        for (std::size_t i = 0; i < shard_count_; ++i) {
            std::lock_guard<std::mutex> lk(shards_[i]->mu);
            shards_[i]->cache.clear();
        }
    }

    // Total stored entries across shards (may include not-yet-purged expired items).
    std::size_t size() const {
        std::size_t total = 0;
        for (std::size_t i = 0; i < shard_count_; ++i) {
            std::lock_guard<std::mutex> lk(shards_[i]->mu);
            total += shards_[i]->cache.size();
        }
        return total;
    }

    std::size_t capacity() const noexcept { return total_capacity_; }
    std::size_t shard_count() const noexcept { return shard_count_; }

private:
    static const std::size_t kShardPad = MYFRAME_CACHE_LINE_SIZE;

    struct Entry {
        Value value;
        time_point expires_at;
        bool has_ttl;
    };

    struct Shard {
        char pad_before[kShardPad];
        mutable std::mutex mu;
        LruCache<Key, Entry, Hash, KeyEqual> cache;
        char pad_after[kShardPad];

        Shard(std::size_t cap, const Hash& hash, const KeyEqual& eq)
            : pad_before(), mu(), cache(cap, hash, eq), pad_after() {}
    };

    static std::size_t validate_capacity(std::size_t cap) {
        if (cap == 0) {
            throw std::invalid_argument("ShardedTtlCache capacity must be > 0");
        }
        return cap;
    }

    static std::size_t compute_shard_count(std::size_t total_cap,
                                           std::size_t requested_shards) {
        if (requested_shards == 0) {
            throw std::invalid_argument("ShardedTtlCache shard count must be > 0");
        }
        static const std::size_t kMaxShards = 65536;
        if (requested_shards > kMaxShards) {
            throw std::invalid_argument(
                "ShardedTtlCache shard count exceeds maximum (65536)");
        }
        std::size_t n = 1;
        while (n < requested_shards) {
            n <<= 1;
        }
        // Never exceed total_cap so every shard has capacity >= 1.
        while (n > total_cap && n > 1) {
            n >>= 1;
        }
        return n;
    }

    static Entry make_entry(const Value& value, std::uint64_t ttl_ms) {
        if (ttl_ms == 0) {
            return Entry{value, time_point(), false};
        }
        return Entry{value, Clock::now() + std::chrono::milliseconds(ttl_ms), true};
    }

    static Entry make_entry_move(Value&& value, std::uint64_t ttl_ms) {
        if (ttl_ms == 0) {
            return Entry{std::move(value), time_point(), false};
        }
        return Entry{
            std::move(value), Clock::now() + std::chrono::milliseconds(ttl_ms), true};
    }

    static bool is_expired(const Entry& e, const time_point& now) noexcept {
        return e.has_ttl && !(now < e.expires_at);
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

    std::size_t total_capacity_;
    std::size_t shard_count_;
    std::size_t shard_mask_;
    Hash hasher_;
    std::vector<std::unique_ptr<Shard> > shards_;
};

} // namespace containers
} // namespace myframe
