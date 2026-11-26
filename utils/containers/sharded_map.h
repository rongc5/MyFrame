#pragma once

#include "../lockfree/detail/cache_line.h"

#include <climits>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace myframe {
namespace containers {

// Thread-safe sharded concurrent hash map.
//
// Splits keys across N power-of-two independent mutex-protected shards
// (default 64) with cache-line padding between shards to reduce lock
// contention and false sharing under multi-threaded server workloads.
//
// Safety:
//   - Never returns raw pointers or iterators that escape a shard lock.
//   - Use visit() / upsert() for atomic in-place mutations under the shard lock.
//   - Callbacks passed to visit() / upsert() / for_each() must NOT call back
//     into the same ShardedMap instance (non-recursive std::mutex).
template <typename Key,
          typename Value,
          typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key> >
class ShardedMap {
public:
    typedef Key key_type;
    typedef Value mapped_type;

    explicit ShardedMap(std::size_t requested_shards = 64,
                        const Hash& hash = Hash(),
                        const KeyEqual& eq = KeyEqual())
        : shard_count_(round_up_shards(requested_shards)),
          shard_mask_(shard_count_ - 1),
          hasher_(hash),
          shards_() {
        shards_.reserve(shard_count_);
        for (std::size_t i = 0; i < shard_count_; ++i) {
            shards_.emplace_back(new Shard(hash, eq));
        }
    }

    ShardedMap(const ShardedMap&) = delete;
    ShardedMap& operator=(const ShardedMap&) = delete;

    // Inserts or overwrites `key`. Returns true if inserted, false if overwritten.
    bool put(const Key& key, const Value& value) {
        Shard& shard = shard_for(key);
        std::lock_guard<std::mutex> lk(shard.mu);
        typename MapType::iterator it = shard.map.find(key);
        if (it != shard.map.end()) {
            it->second = value;
            return false;
        }
        shard.map.emplace(key, value);
        return true;
    }

    bool put(const Key& key, Value&& value) {
        Shard& shard = shard_for(key);
        std::lock_guard<std::mutex> lk(shard.mu);
        typename MapType::iterator it = shard.map.find(key);
        if (it != shard.map.end()) {
            it->second = std::move(value);
            return false;
        }
        shard.map.emplace(key, std::move(value));
        return true;
    }

    // Inserts only if `key` is absent. Returns true if inserted.
    bool insert_if_absent(const Key& key, const Value& value) {
        Shard& shard = shard_for(key);
        std::lock_guard<std::mutex> lk(shard.mu);
        return shard.map.emplace(key, value).second;
    }

    bool insert_if_absent(const Key& key, Value&& value) {
        Shard& shard = shard_for(key);
        std::lock_guard<std::mutex> lk(shard.mu);
        return shard.map.emplace(key, std::move(value)).second;
    }

    bool get(const Key& key, Value& out) const {
        const Shard& shard = shard_for(key);
        std::lock_guard<std::mutex> lk(shard.mu);
        typename MapType::const_iterator it = shard.map.find(key);
        if (it == shard.map.end()) {
            return false;
        }
        out = it->second;
        return true;
    }

    bool contains(const Key& key) const {
        const Shard& shard = shard_for(key);
        std::lock_guard<std::mutex> lk(shard.mu);
        return shard.map.find(key) != shard.map.end();
    }

    bool erase(const Key& key) {
        Shard& shard = shard_for(key);
        std::lock_guard<std::mutex> lk(shard.mu);
        return shard.map.erase(key) > 0;
    }

    // Atomically invokes `fn(Value&)` under the shard lock if `key` exists.
    template <typename Fn>
    bool visit(const Key& key, Fn&& fn) {
        Shard& shard = shard_for(key);
        std::lock_guard<std::mutex> lk(shard.mu);
        typename MapType::iterator it = shard.map.find(key);
        if (it == shard.map.end()) {
            return false;
        }
        fn(it->second);
        return true;
    }

    template <typename Fn>
    bool visit(const Key& key, Fn&& fn) const {
        const Shard& shard = shard_for(key);
        std::lock_guard<std::mutex> lk(shard.mu);
        typename MapType::const_iterator it = shard.map.find(key);
        if (it == shard.map.end()) {
            return false;
        }
        fn(it->second);
        return true;
    }

    // Invokes `fn(Value&, bool inserted)` while holding the shard lock.
    // On a miss, default-constructs a Value first; if `fn` throws after a fresh
    // insert, the newly inserted entry is rolled back before rethrowing.
    template <typename Fn>
    void upsert(const Key& key, Fn&& fn) {
        Shard& shard = shard_for(key);
        std::lock_guard<std::mutex> lk(shard.mu);
        typename MapType::iterator it = shard.map.find(key);
        if (it != shard.map.end()) {
            fn(it->second, false);
            return;
        }
        std::pair<typename MapType::iterator, bool> res =
            shard.map.emplace(key, Value());
        try {
            fn(res.first->second, true);
        } catch (...) {
            shard.map.erase(res.first);
            throw;
        }
    }

    // Visits every entry shard-by-shard, locking one shard at a time.
    // `fn` runs while holding each shard's lock (blocking concurrent writers to
    // that shard) and sees per-shard state rather than a global atomic snapshot.
    // `fn` is called as `fn(const Key&, const Value&)`.
    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (std::size_t i = 0; i < shard_count_; ++i) {
            std::lock_guard<std::mutex> lk(shards_[i]->mu);
            for (typename MapType::const_iterator it = shards_[i]->map.begin();
                 it != shards_[i]->map.end();
                 ++it) {
                fn(it->first, it->second);
            }
        }
    }

    void clear() {
        for (std::size_t i = 0; i < shard_count_; ++i) {
            std::lock_guard<std::mutex> lk(shards_[i]->mu);
            shards_[i]->map.clear();
        }
    }

    // Exact when quiescent; approximate sum across shards under concurrent writes.
    std::size_t size() const {
        std::size_t total = 0;
        for (std::size_t i = 0; i < shard_count_; ++i) {
            std::lock_guard<std::mutex> lk(shards_[i]->mu);
            total += shards_[i]->map.size();
        }
        return total;
    }

    std::size_t shard_count() const noexcept {
        return shard_count_;
    }

private:
    static const std::size_t kShardPad = MYFRAME_CACHE_LINE_SIZE;
    typedef std::unordered_map<Key, Value, Hash, KeyEqual> MapType;

    struct Shard {
        char pad_before[kShardPad];
        mutable std::mutex mu;
        MapType map;
        char pad_after[kShardPad];

        Shard(const Hash& hash, const KeyEqual& eq)
            : pad_before(), mu(), map(0, hash, eq), pad_after() {}
    };

    static std::size_t round_up_shards(std::size_t requested) {
        if (requested == 0) {
            throw std::invalid_argument("ShardedMap shard count must be > 0");
        }
        static const std::size_t kMaxShards = 65536;
        if (requested > kMaxShards) {
            throw std::invalid_argument("ShardedMap shard count exceeds maximum (65536)");
        }
        std::size_t n = 1;
        while (n < requested) {
            n <<= 1;
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

    const Shard& shard_for(const Key& key) const {
        return *shards_[shard_index(key)];
    }

    std::size_t shard_count_;
    std::size_t shard_mask_;
    Hash hasher_;
    std::vector<std::unique_ptr<Shard> > shards_;
};

} // namespace containers
} // namespace myframe
