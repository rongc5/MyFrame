#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace myframe {
namespace containers {

// Single-threaded O(1) Least-Recently-Used (LRU) cache.
//
// Design:
//   - Backed by std::list<std::pair<const Key, Value>> for O(1) recency splicing
//     and std::unordered_map<const Key*, list::iterator> to avoid duplicating Key
//     storage across both containers.
//   - Keys live inside std::list nodes, whose addresses remain stable for the
//     entire lifetime of the entry.
//   - Not thread-safe; guard externally with a mutex (or build an array of
//     {mutex, LruCache} shards) for concurrent access.
template <typename Key,
          typename Value,
          typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key> >
class LruCache {
public:
    typedef Key key_type;
    typedef Value mapped_type;
    typedef std::pair<const Key, Value> entry_type;

    explicit LruCache(std::size_t capacity,
                      const Hash& hash = Hash(),
                      const KeyEqual& eq = KeyEqual())
        : capacity_(validate_capacity(capacity)),
          list_(),
          map_(0, PtrHash(hash), PtrEqual(eq)) {
        // Reserve capacity + 1 for small caches so the first eviction never
        // triggers a rehash, while capping initial memory for huge capacities.
        static const std::size_t kMaxInitialReserve = 4096;
        const std::size_t initial =
            (capacity_ < kMaxInitialReserve) ? (capacity_ + 1) : kMaxInitialReserve;
        map_.reserve(initial);
    }

    LruCache(const LruCache&) = delete;
    LruCache& operator=(const LruCache&) = delete;

    // Inserts or updates `key`. Moves `key` to MRU and evicts LRU if full.
    // Returns true if a new key was inserted, false if an existing key was updated.
    bool put(const Key& key, const Value& value) {
        typename MapType::iterator it = map_.find(&key);
        if (it != map_.end()) {
            it->second->second = value;
            list_.splice(list_.begin(), list_, it->second);
            return false;
        }
        list_.emplace_front(key, value);
        insert_front_node_or_rollback();
        evict_if_needed();
        return true;
    }

    bool put(const Key& key, Value&& value) {
        typename MapType::iterator it = map_.find(&key);
        if (it != map_.end()) {
            it->second->second = std::move(value);
            list_.splice(list_.begin(), list_, it->second);
            return false;
        }
        list_.emplace_front(key, std::move(value));
        insert_front_node_or_rollback();
        evict_if_needed();
        return true;
    }

    // Constructs Value in-place. Supports move-only / non-copyable Value types.
    // Returns true if inserted new key, false if existing key was updated.
    template <typename... Args>
    bool emplace(const Key& key, Args&&... args) {
        typename MapType::iterator it = map_.find(&key);
        if (it != map_.end()) {
            Value tmp(std::forward<Args>(args)...);
            it->second->second = std::move(tmp);
            list_.splice(list_.begin(), list_, it->second);
            return false;
        }
        list_.emplace_front(std::piecewise_construct,
                            std::forward_as_tuple(key),
                            std::forward_as_tuple(std::forward<Args>(args)...));
        insert_front_node_or_rollback();
        evict_if_needed();
        return true;
    }

    // Copies the value into `out` and marks `key` as MRU. Returns false on miss.
    bool get(const Key& key, Value& out) {
        typename MapType::iterator it = map_.find(&key);
        if (it == map_.end()) {
            return false;
        }
        list_.splice(list_.begin(), list_, it->second);
        out = it->second->second;
        return true;
    }

    // Zero-copy lookup: returns pointer to cached value and marks `key` as MRU,
    // or nullptr on miss. Pointer stays valid until `key` is evicted/erased/cleared.
    Value* get_ptr(const Key& key) {
        typename MapType::iterator it = map_.find(&key);
        if (it == map_.end()) {
            return nullptr;
        }
        list_.splice(list_.begin(), list_, it->second);
        return &it->second->second;
    }

    // Lookup without altering LRU recency. Returns nullptr on miss.
    const Value* peek_ptr(const Key& key) const {
        typename MapType::const_iterator it = map_.find(&key);
        if (it == map_.end()) {
            return nullptr;
        }
        return &it->second->second;
    }

    bool contains(const Key& key) const {
        return map_.find(&key) != map_.end();
    }

    bool erase(const Key& key) {
        typename MapType::iterator it = map_.find(&key);
        if (it == map_.end()) {
            return false;
        }
        typename ListType::iterator list_it = it->second;
        map_.erase(it);
        list_.erase(list_it);
        return true;
    }

    // Removes all entries for which `pred(const Key&, const Value&)` returns true.
    // Returns number of removed entries.
    template <typename Pred>
    std::size_t erase_if(Pred&& pred) {
        std::size_t removed = 0;
        for (typename ListType::iterator it = list_.begin(); it != list_.end(); ) {
            if (pred(it->first, it->second)) {
                typename ListType::iterator victim = it++;
                map_.erase(&victim->first);
                list_.erase(victim);
                ++removed;
            } else {
                ++it;
            }
        }
        return removed;
    }

    void clear() noexcept {
        map_.clear();
        list_.clear();
    }

    std::size_t size() const noexcept { return map_.size(); }
    std::size_t capacity() const noexcept { return capacity_; }
    bool empty() const noexcept { return map_.empty(); }

private:
    typedef std::list<entry_type> ListType;

    struct PtrHash {
        Hash hasher;
        explicit PtrHash(const Hash& h) : hasher(h) {}
        std::size_t operator()(const Key* k) const {
            return hasher(*k);
        }
    };

    struct PtrEqual {
        KeyEqual eq;
        explicit PtrEqual(const KeyEqual& e) : eq(e) {}
        bool operator()(const Key* a, const Key* b) const {
            return a == b || eq(*a, *b);
        }
    };

    typedef std::unordered_map<const Key*, typename ListType::iterator, PtrHash, PtrEqual>
        MapType;

    static std::size_t validate_capacity(std::size_t cap) {
        if (cap == 0) {
            throw std::invalid_argument("LruCache capacity must be > 0");
        }
        return cap;
    }

    void insert_front_node_or_rollback() {
        try {
            map_.emplace(&list_.front().first, list_.begin());
        } catch (...) {
            list_.pop_front();
            throw;
        }
    }

    void evict_if_needed() {
        while (map_.size() > capacity_) {
            const Key* victim = &list_.back().first;
            map_.erase(victim);
            list_.pop_back();
        }
    }

    std::size_t capacity_;
    ListType list_;
    MapType map_;
};

} // namespace containers
} // namespace myframe
