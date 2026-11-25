#pragma once

#include "lru_cache.h"

#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <ratio>
#include <stdexcept>
#include <type_traits>
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
    // If `Clock::now() + ttl_ms` would exceed `Clock::time_point::max()`, the
    // expiration timestamp safely saturates to `Clock::time_point::max()` without
    // signed integer overflow.
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
    typedef typename Clock::duration clock_duration;
    typedef typename clock_duration::rep clock_rep;
    typedef typename clock_duration::period clock_period;
    typedef std::ratio_divide<std::milli, clock_period> ms_to_ticks_ratio;

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

    // Converts `ttl_ms` to clock ticks without signed or unsigned overflow.
    // Returns false if the result exceeds UINT64_MAX ticks.
    static bool ms_to_delta_ticks(std::uint64_t ttl_ms,
                                  std::uint64_t& out_ticks,
                                  std::true_type /*den == 1*/) noexcept {
        static const std::uint64_t kNum =
            static_cast<std::uint64_t>(ms_to_ticks_ratio::num);
        if (ttl_ms > std::numeric_limits<std::uint64_t>::max() / kNum) {
            return false;
        }
        out_ticks = ttl_ms * kNum;
        return true;
    }

    static bool ms_to_delta_ticks(std::uint64_t ttl_ms,
                                  std::uint64_t& out_ticks,
                                  std::false_type /*den > 1*/) noexcept {
        static const std::uint64_t kNum =
            static_cast<std::uint64_t>(ms_to_ticks_ratio::num);
        static const std::uint64_t kDen =
            static_cast<std::uint64_t>(ms_to_ticks_ratio::den);
        const std::uint64_t q = ttl_ms / kDen;
        const std::uint64_t r = ttl_ms % kDen;
        if (q > std::numeric_limits<std::uint64_t>::max() / kNum) {
            return false;
        }
        const std::uint64_t p1 = q * kNum;
        const std::uint64_t p2 =
            (r <= std::numeric_limits<std::uint64_t>::max() / kNum)
                ? ((r * kNum) / kDen)
                : static_cast<std::uint64_t>(
                      (static_cast<long double>(r) * static_cast<long double>(kNum)) /
                      static_cast<long double>(kDen));
        if (p1 > std::numeric_limits<std::uint64_t>::max() - p2) {
            return false;
        }
        out_ticks = p1 + p2;
        return true;
    }

    // Adds `delta_ticks` to `now_ticks` given the precondition that the exact
    // mathematical sum is strictly less than `time_point::max()`.
    static clock_rep add_ticks_in_range(clock_rep now_ticks,
                                        std::uint64_t delta_ticks,
                                        std::true_type /*signed rep*/) noexcept {
        static const clock_rep kMaxRep = std::numeric_limits<clock_rep>::max();
        static const std::uint64_t kMaxRepU64 = static_cast<std::uint64_t>(kMaxRep);
        if (delta_ticks <= kMaxRepU64) {
            return static_cast<clock_rep>(now_ticks + static_cast<clock_rep>(delta_ticks));
        }
        // Only reachable when now_ticks < 0 and delta_ticks > kMaxRep: split the
        // addition into two signed-overflow-free steps.
        const clock_rep step2 = static_cast<clock_rep>(delta_ticks - kMaxRepU64);
        const clock_rep mid = static_cast<clock_rep>(now_ticks + kMaxRep);
        return static_cast<clock_rep>(mid + step2);
    }

    static clock_rep add_ticks_in_range(clock_rep now_ticks,
                                        std::uint64_t delta_ticks,
                                        std::false_type /*unsigned rep*/) noexcept {
        return static_cast<clock_rep>(now_ticks + static_cast<clock_rep>(delta_ticks));
    }

    static time_point compute_expiry_impl(time_point now,
                                          std::uint64_t ttl_ms,
                                          std::true_type /*integral rep*/) noexcept {
        static_assert(sizeof(clock_rep) <= sizeof(std::uint64_t),
                      "ShardedTtlCache clock_rep wider than 64 bits is unsupported");
        std::uint64_t delta_ticks = 0;
        if (!ms_to_delta_ticks(
                ttl_ms,
                delta_ticks,
                std::integral_constant<bool, (ms_to_ticks_ratio::den == 1)>())) {
            return time_point::max();
        }
        const clock_rep now_ticks = now.time_since_epoch().count();
        const clock_rep max_ticks = time_point::max().time_since_epoch().count();
        typedef typename std::make_unsigned<clock_rep>::type unsigned_rep;
        const unsigned_rep headroom_ticks =
            static_cast<unsigned_rep>(max_ticks) - static_cast<unsigned_rep>(now_ticks);
        if (delta_ticks >= static_cast<std::uint64_t>(headroom_ticks)) {
            return time_point::max();
        }
        const clock_rep expiry_ticks =
            add_ticks_in_range(now_ticks, delta_ticks, std::is_signed<clock_rep>());
        return time_point(clock_duration(expiry_ticks));
    }

    static time_point compute_expiry_impl(time_point now,
                                          std::uint64_t ttl_ms,
                                          std::false_type /*floating rep*/) noexcept {
        const long double max_t =
            static_cast<long double>(time_point::max().time_since_epoch().count());
        const long double now_t =
            static_cast<long double>(now.time_since_epoch().count());
        const long double delta =
            static_cast<long double>(ttl_ms) *
            (static_cast<long double>(ms_to_ticks_ratio::num) /
             static_cast<long double>(ms_to_ticks_ratio::den));
        if (!(delta < max_t - now_t)) {
            return time_point::max();
        }
        return time_point(clock_duration(static_cast<clock_rep>(now_t + delta)));
    }

    static time_point compute_expiry(time_point now, std::uint64_t ttl_ms) noexcept {
        return compute_expiry_impl(now, ttl_ms, std::is_integral<clock_rep>());
    }

    static Entry make_entry(const Value& value, std::uint64_t ttl_ms) {
        if (ttl_ms == 0) {
            return Entry{value, time_point(), false};
        }
        return Entry{value, compute_expiry(Clock::now(), ttl_ms), true};
    }

    static Entry make_entry_move(Value&& value, std::uint64_t ttl_ms) {
        if (ttl_ms == 0) {
            return Entry{std::move(value), time_point(), false};
        }
        return Entry{
            std::move(value), compute_expiry(Clock::now(), ttl_ms), true};
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
