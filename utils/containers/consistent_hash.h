#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace myframe {
namespace containers {

namespace detail {

inline std::uint64_t ch_mix64(std::uint64_t x) noexcept {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

// Deterministic FNV-1a 64-bit followed by SplitMix64 finalizer.
inline std::uint64_t ch_hash_bytes(const void* data, std::size_t len) noexcept {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    std::uint64_t h = 14695981039346656037ULL;
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<std::uint64_t>(p[i]);
        h *= 1099511628211ULL;
    }
    return ch_mix64(h ^ static_cast<std::uint64_t>(len));
}

template <typename T, typename = void>
struct HasLess : std::false_type {};

template <typename T>
struct HasLess<T, typename std::enable_if<
    std::is_convertible<
        decltype(std::declval<const T&>() < std::declval<const T&>()),
        bool>::value>::type> : std::true_type {};

} // namespace detail

// Default node hasher for ConsistentHash.
//   - For std::string, computes a deterministic, platform-independent 64-bit
//     FNV-1a + SplitMix64 hash (independent of std::hash<std::string>).
//   - For other node types, delegates to std::hash<Node>; callers requiring
//     cross-platform ring portability for non-string nodes should supply a
//     deterministic custom Hash functor.
template <typename Node>
struct DefaultNodeHash {
    std::size_t operator()(const Node& node) const {
        return std::hash<Node>()(node);
    }
};

template <>
struct DefaultNodeHash<std::string> {
    static std::uint64_t hash64(const std::string& node) noexcept {
        return detail::ch_hash_bytes(node.data(), node.size());
    }

    std::size_t operator()(const std::string& node) const noexcept {
        return static_cast<std::size_t>(hash64(node));
    }
};

namespace detail {

template <typename Node, typename Hash>
struct NodeSeedHelper {
    static std::uint64_t seed(const Hash& hasher, const Node& node) {
        return ch_mix64(static_cast<std::uint64_t>(hasher(node)));
    }
};

template <>
struct NodeSeedHelper<std::string, DefaultNodeHash<std::string> > {
    static std::uint64_t seed(const DefaultNodeHash<std::string>&,
                              const std::string& node) noexcept {
        return DefaultNodeHash<std::string>::hash64(node);
    }
};

} // namespace detail

// Weighted virtual-node consistent hash ring.
//
// Common ads/search uses:
//   - Search index shard routing and distributed cache node selection
//   - Replica / backup node selection via get_nodes(key, count)
//   - Minimal key remapping (~1/N) on cluster scale-out / failover
//
// Properties:
//   - Strict C++11/C++17 compatible, deterministic platform-independent 64-bit
//     FNV-1a + SplitMix64 hashing for both lookup keys and default std::string
//     nodes (via DefaultNodeHash<std::string>).
//   - Non-std::string defaults (std::hash<Node>) and explicit custom Hash
//     functors remain supported; callers supplying custom/non-string hashes are
//     responsible for cross-platform hash determinism when portability matters.
//   - Supports node weights (vnodes = weight * virtual_nodes_per_weight), with
//     upfront overflow validation (std::overflow_error) and transactional
//     updates (strong exception guarantee: failures never drop existing nodes
//     or leave orphan ring points).
//   - Ring-point collisions are stored without insertion-order-dependent
//     salting: colliding nodes at the same 64-bit ring point are ordered by
//     operator< when Node supports < (making routing strictly invariant to
//     insertion/removal order), or by insertion order for custom Node types
//     without operator<.
//   - Thread-safe for concurrent reads; pair with DoubleBuffer<ConsistentHash<...>>
//     for lock-free-read hot topology reloads.
template <typename Node = std::string,
          typename Hash = DefaultNodeHash<Node>,
          typename KeyEqual = std::equal_to<Node> >
class ConsistentHash {
public:
    typedef Node node_type;
    static const std::size_t kDefaultMaxTotalVnodes = 4194304; // 4M virtual nodes safety cap

    explicit ConsistentHash(std::size_t virtual_nodes_per_weight = 160,
                            const Hash& node_hash = Hash(),
                            const KeyEqual& node_eq = KeyEqual(),
                            std::size_t max_total_vnodes = kDefaultMaxTotalVnodes)
        : vnodes_per_weight_(validate_vnodes(virtual_nodes_per_weight)),
          max_total_vnodes_(validate_max_vnodes(max_total_vnodes)),
          node_hash_(node_hash),
          node_eq_(node_eq),
          ring_(),
          node_points_(0, node_hash, node_eq) {}

    // Adds a node with the given positive weight, or updates its weight if it
    // already exists. Returns true if newly added, false if updated.
    // Throws std::invalid_argument if weight == 0, std::overflow_error if
    // weight * virtual_nodes_per_weight overflows std::size_t, or
    // std::length_error if total virtual nodes would exceed max_total_vnodes.
    // Strong exception guarantee: on any exception, existing ring/node state is
    // completely preserved with no orphan ring points.
    bool add_node(const Node& node, std::size_t weight = 1) {
        if (weight == 0) {
            throw std::invalid_argument("ConsistentHash node weight must be > 0");
        }
        if (weight > std::numeric_limits<std::size_t>::max() / vnodes_per_weight_) {
            throw std::overflow_error("ConsistentHash virtual node count overflow");
        }
        const std::size_t count = weight * vnodes_per_weight_;

        typename NodePointsMap::iterator existing_it = node_points_.find(node);
        const bool existed = (existing_it != node_points_.end());
        const std::size_t old_count = existed ? existing_it->second.size() : 0;

        if (existed && old_count == count) {
            return false;
        }

        if (existed && count < old_count) {
            // Shrink weight: locate all iterators to erase before mutating so even
            // a throwing KeyEqual preserves strong exception safety.
            std::vector<typename RingMap::iterator> to_erase;
            to_erase.reserve(old_count - count);
            const Node& stored_node = existing_it->first;
            for (std::size_t i = count; i < old_count; ++i) {
                to_erase.push_back(find_ring_point(existing_it->second[i], stored_node));
            }
            // Commit (nothrow):
            for (std::size_t i = 0; i < to_erase.size(); ++i) {
                if (to_erase[i] != ring_.end()) {
                    ring_.erase(to_erase[i]);
                }
            }
            existing_it->second.resize(count);
            return false;
        }

        // New node or growing weight: prepare all point hashes and rollback storage
        // before mutating ring_ or node_points_.
        const std::size_t add_begin = old_count;
        const std::size_t add_count = count - old_count;
        if (add_count > max_total_vnodes_ ||
            ring_.size() > max_total_vnodes_ - add_count) {
            throw std::length_error(
                "ConsistentHash total virtual nodes exceeds configured limit");
        }

        std::vector<std::uint64_t> new_points;
        new_points.reserve(count);
        if (existed) {
            new_points.assign(existing_it->second.begin(), existing_it->second.end());
        }
        const std::uint64_t base_seed =
            compute_node_seed(existed ? existing_it->first : node);
        for (std::size_t i = add_begin; i < count; ++i) {
            new_points.push_back(vnode_hash(base_seed, static_cast<std::uint64_t>(i)));
        }

        std::vector<typename RingMap::iterator> inserted_iters;
        inserted_iters.reserve(add_count);

        typename NodePointsMap::iterator target_it = existing_it;
        if (!existed) {
            target_it = node_points_.emplace(node, std::move(new_points)).first;
        }

        const Node& stored_node = target_it->first;
        const std::vector<std::uint64_t>& pts = existed ? new_points : target_it->second;
        try {
            for (std::size_t i = add_begin; i < count; ++i) {
                inserted_iters.push_back(insert_ring_point(pts[i], stored_node));
            }
        } catch (...) {
            for (std::size_t j = 0; j < inserted_iters.size(); ++j) {
                ring_.erase(inserted_iters[j]);
            }
            if (!existed) {
                node_points_.erase(target_it);
            }
            throw;
        }

        if (existed) {
            existing_it->second.swap(new_points);
        }
        return !existed;
    }

    // Removes a node and all of its virtual nodes from the ring.
    // Returns true if the node existed and was removed, false otherwise.
    // Strong exception guarantee: prepares all ring iterators before mutating so
    // any exception from lookup, allocation, or KeyEqual leaves state unchanged.
    bool remove_node(const Node& node) {
        typename NodePointsMap::iterator it = node_points_.find(node);
        if (it == node_points_.end()) {
            return false;
        }
        const Node& stored_node = it->first;
        const std::vector<std::uint64_t>& points = it->second;
        std::vector<typename RingMap::iterator> to_erase;
        to_erase.reserve(points.size());
        for (std::size_t i = 0; i < points.size(); ++i) {
            to_erase.push_back(find_ring_point(points[i], stored_node));
        }
        // Commit (nothrow):
        for (std::size_t i = 0; i < to_erase.size(); ++i) {
            if (to_erase[i] != ring_.end()) {
                ring_.erase(to_erase[i]);
            }
        }
        node_points_.erase(it);
        return true;
    }

    bool contains_node(const Node& node) const {
        return node_points_.find(node) != node_points_.end();
    }

    // Selects the primary node responsible for `key`. Returns false if ring is empty.
    bool get_node(const void* data, std::size_t len, Node& out) const {
        if (ring_.empty()) {
            return false;
        }
        const std::uint64_t h = hash_bytes(data, len);
        typename RingMap::const_iterator it = ring_.lower_bound(h);
        if (it == ring_.end()) {
            it = ring_.begin();
        }
        out = it->second;
        return true;
    }

    bool get_node(const std::string& key, Node& out) const {
        return get_node(key.data(), key.size(), out);
    }

    bool get_node_u64(std::uint64_t key, Node& out) const {
        const std::uint64_t h = mix64(key);
        if (ring_.empty()) {
            return false;
        }
        typename RingMap::const_iterator it = ring_.lower_bound(h);
        if (it == ring_.end()) {
            it = ring_.begin();
        }
        out = it->second;
        return true;
    }

    // Walks clockwise from hash(key) and returns up to min(count, node_count())
    // DISTINCT physical nodes (primary + replicas).
    // Uses zero-allocation linear deduplication for small replica counts (<= 16).
    std::vector<Node> get_nodes(const void* data,
                                std::size_t len,
                                std::size_t count) const {
        std::vector<Node> result;
        if (ring_.empty() || count == 0) {
            return result;
        }
        const std::size_t target =
            (count < node_points_.size()) ? count : node_points_.size();
        result.reserve(target);

        const std::uint64_t h = hash_bytes(data, len);
        typename RingMap::const_iterator it = ring_.lower_bound(h);
        if (it == ring_.end()) {
            it = ring_.begin();
        }

        const std::size_t max_steps = ring_.size();
        if (target <= 16) {
            for (std::size_t step = 0; step < max_steps && result.size() < target; ++step) {
                bool dup = false;
                for (std::size_t j = 0; j < result.size(); ++j) {
                    if (node_eq_(result[j], it->second)) {
                        dup = true;
                        break;
                    }
                }
                if (!dup) {
                    result.push_back(it->second);
                }
                ++it;
                if (it == ring_.end()) {
                    it = ring_.begin();
                }
            }
            return result;
        }

        std::unordered_set<Node, Hash, KeyEqual> seen(0, node_hash_, node_eq_);
        seen.reserve(target);
        for (std::size_t step = 0; step < max_steps && result.size() < target; ++step) {
            if (seen.insert(it->second).second) {
                result.push_back(it->second);
            }
            ++it;
            if (it == ring_.end()) {
                it = ring_.begin();
            }
        }
        return result;
    }

    std::vector<Node> get_nodes(const std::string& key, std::size_t count) const {
        return get_nodes(key.data(), key.size(), count);
    }

    void clear() noexcept {
        ring_.clear();
        node_points_.clear();
    }

    std::size_t node_count() const noexcept { return node_points_.size(); }
    std::size_t vnode_count() const noexcept { return ring_.size(); }
    bool empty() const noexcept { return ring_.empty(); }

private:
    typedef std::multimap<std::uint64_t, Node> RingMap;
    typedef std::unordered_map<Node, std::vector<std::uint64_t>, Hash, KeyEqual>
        NodePointsMap;

    static std::size_t validate_vnodes(std::size_t v) {
        if (v == 0) {
            throw std::invalid_argument(
                "ConsistentHash virtual_nodes_per_weight must be > 0");
        }
        return v;
    }

    static std::size_t validate_max_vnodes(std::size_t m) {
        if (m == 0) {
            throw std::invalid_argument(
                "ConsistentHash max_total_vnodes must be > 0");
        }
        return m;
    }

    static std::uint64_t mix64(std::uint64_t x) noexcept {
        return detail::ch_mix64(x);
    }

    // Bijective in `index` for any fixed `seed`: a single node's virtual nodes
    // are mathematically guaranteed never to collide with each other.
    static std::uint64_t vnode_hash(std::uint64_t seed,
                                    std::uint64_t index) noexcept {
        return mix64(seed ^ mix64(index + 0x517cc1b727220a95ULL));
    }

    static std::uint64_t hash_bytes(const void* data, std::size_t len) noexcept {
        return detail::ch_hash_bytes(data, len);
    }

    std::uint64_t compute_node_seed(const Node& node) const {
        return detail::NodeSeedHelper<Node, Hash>::seed(node_hash_, node);
    }

    typename RingMap::iterator insert_ring_point(std::uint64_t h, const Node& node) {
        return insert_ring_point_impl(h, node, detail::HasLess<Node>());
    }

    typename RingMap::iterator insert_ring_point_impl(std::uint64_t h,
                                                      const Node& node,
                                                      std::true_type) {
        std::pair<typename RingMap::iterator, typename RingMap::iterator> range =
            ring_.equal_range(h);
        typename RingMap::iterator pos = range.first;
        while (pos != range.second && (pos->second < node)) {
            ++pos;
        }
        return ring_.emplace_hint(pos, h, node);
    }

    typename RingMap::iterator insert_ring_point_impl(std::uint64_t h,
                                                      const Node& node,
                                                      std::false_type) {
        return ring_.emplace_hint(ring_.upper_bound(h), h, node);
    }

    typename RingMap::iterator find_ring_point(std::uint64_t h, const Node& node) {
        std::pair<typename RingMap::iterator, typename RingMap::iterator> range =
            ring_.equal_range(h);
        for (typename RingMap::iterator it = range.first; it != range.second; ++it) {
            if (node_eq_(it->second, node)) {
                return it;
            }
        }
        return ring_.end();
    }

    std::size_t vnodes_per_weight_;
    std::size_t max_total_vnodes_;
    Hash node_hash_;
    KeyEqual node_eq_;
    RingMap ring_;
    NodePointsMap node_points_;
};

} // namespace containers
} // namespace myframe
