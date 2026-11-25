#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace myframe {
namespace containers {

// Weighted virtual-node consistent hash ring.
//
// Common ads/search uses:
//   - Search index shard routing and distributed cache node selection
//   - Replica / backup node selection via get_nodes(key, count)
//   - Minimal key remapping (~1/N) on cluster scale-out / failover
//
// Properties:
//   - Strict C++11, deterministic platform-independent 64-bit hash mixing.
//   - Supports node weights (vnodes = weight * virtual_nodes_per_weight).
//   - Thread-safe for concurrent reads; pair with DoubleBuffer<ConsistentHash<...>>
//     for lock-free hot topology reloads.
template <typename Node = std::string,
          typename Hash = std::hash<Node>,
          typename KeyEqual = std::equal_to<Node> >
class ConsistentHash {
public:
    typedef Node node_type;

    explicit ConsistentHash(std::size_t virtual_nodes_per_weight = 160,
                            const Hash& node_hash = Hash(),
                            const KeyEqual& node_eq = KeyEqual())
        : vnodes_per_weight_(validate_vnodes(virtual_nodes_per_weight)),
          node_hash_(node_hash),
          node_eq_(node_eq),
          ring_(),
          node_points_(0, node_hash, node_eq) {}

    // Adds a node with the given positive weight, or updates its weight if it
    // already exists. Returns true if newly added, false if updated.
    bool add_node(const Node& node, std::size_t weight = 1) {
        if (weight == 0) {
            throw std::invalid_argument("ConsistentHash node weight must be > 0");
        }
        const bool existed = remove_node(node);

        const std::size_t count = weight * vnodes_per_weight_;
        std::vector<std::uint64_t> points;
        points.reserve(count);

        const std::uint64_t base_seed = mix64(static_cast<std::uint64_t>(node_hash_(node)));
        std::uint64_t salt = 0;
        for (std::size_t i = 0; i < count; ++i) {
            std::uint64_t h = vnode_hash(base_seed, static_cast<std::uint64_t>(i), salt);
            while (ring_.find(h) != ring_.end()) {
                ++salt;
                h = vnode_hash(base_seed, static_cast<std::uint64_t>(i), salt);
            }
            ring_.emplace(h, node);
            points.push_back(h);
        }

        node_points_.emplace(node, std::move(points));
        return !existed;
    }

    bool remove_node(const Node& node) {
        typename NodePointsMap::iterator it = node_points_.find(node);
        if (it == node_points_.end()) {
            return false;
        }
        const std::vector<std::uint64_t>& points = it->second;
        for (std::size_t i = 0; i < points.size(); ++i) {
            ring_.erase(points[i]);
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

        std::unordered_set<Node, Hash, KeyEqual> seen(target * 2, node_hash_, node_eq_);
        const std::uint64_t h = hash_bytes(data, len);
        typename RingMap::const_iterator it = ring_.lower_bound(h);
        if (it == ring_.end()) {
            it = ring_.begin();
        }

        const std::size_t max_steps = ring_.size();
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
    typedef std::map<std::uint64_t, Node> RingMap;
    typedef std::unordered_map<Node, std::vector<std::uint64_t>, Hash, KeyEqual>
        NodePointsMap;

    static std::size_t validate_vnodes(std::size_t v) {
        if (v == 0) {
            throw std::invalid_argument(
                "ConsistentHash virtual_nodes_per_weight must be > 0");
        }
        return v;
    }

    static std::uint64_t mix64(std::uint64_t x) noexcept {
        x += 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    }

    static std::uint64_t vnode_hash(std::uint64_t seed,
                                    std::uint64_t index,
                                    std::uint64_t salt) noexcept {
        std::uint64_t h = seed ^ mix64(index + 0x517cc1b727220a95ULL);
        if (salt != 0) {
            h ^= mix64(salt * 0x9e3779b97f4a7c15ULL);
        }
        return mix64(h);
    }

    // Deterministic FNV-1a 64-bit followed by SplitMix64 finalizer.
    static std::uint64_t hash_bytes(const void* data, std::size_t len) noexcept {
        const unsigned char* p = static_cast<const unsigned char*>(data);
        std::uint64_t h = 14695981039346656037ULL;
        for (std::size_t i = 0; i < len; ++i) {
            h ^= static_cast<std::uint64_t>(p[i]);
            h *= 1099511628211ULL;
        }
        return mix64(h ^ static_cast<std::uint64_t>(len));
    }

    std::size_t vnodes_per_weight_;
    Hash node_hash_;
    KeyEqual node_eq_;
    RingMap ring_;
    NodePointsMap node_points_;
};

} // namespace containers
} // namespace myframe
