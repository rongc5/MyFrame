#pragma once

#include "parallel_pipeline.h"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace myframe {
namespace pipeline {

namespace detail {

template <typename Vec>
inline void reserve_geometric(Vec& vec, std::size_t min_capacity) {
    if (vec.capacity() >= min_capacity) {
        return;
    }
    const std::size_t max_cap = vec.max_size();
    if (min_capacity > max_cap) {
        throw std::length_error("Dag vector capacity exceeded");
    }
    std::size_t new_cap = (vec.capacity() == 0) ? std::size_t(4) : vec.capacity();
    if (new_cap > max_cap / 2) {
        new_cap = max_cap;
    } else {
        new_cap *= 2;
    }
    if (new_cap < min_capacity) {
        new_cap = min_capacity;
    }
    vec.reserve(new_cap);
}

} // namespace detail

// Directed Acyclic Graph (DAG) dependency orchestration utility.
//
// Common serving/pipeline uses:
//   - Recall -> pre-rank -> rank -> rerank operator scheduling
//   - Feature / config / module startup dependency validation
//   - Layered topological sort (Kahn BFS) for parallel stage execution
//   - Exact cycle path reporting (iterative 3-color DFS) for diagnostics
//   - Dynamic ready-queue parallel execution (standalone threads or TaskExecutor)
//
// Edge direction convention:
//   - `add_edge(from, to)`: directed edge `from -> to` (`from` executes before `to`).
//   - `add_dependency(node, depends_on)`: equivalent to `add_edge(depends_on, node)`
//     (`depends_on` executes before `node`).
//
// Properties:
//   - Strict C++11/C++17, header-only, deterministic traversal/topological ordering.
//   - 100% iterative algorithms (no recursion stack overflow on deep dependency chains).
//   - Strong exception safety on add_node(), add_edge(), and copy assignment
//     (assuming Hash/KeyEqual have non-throwing swap).
//   - Const methods are thread-safe for concurrent reads (compatible with DoubleBuffer<Dag>).
template <typename Key = std::string,
          typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key> >
class Dag {
public:
    typedef Key key_type;
    typedef std::uint32_t NodeId;

    explicit Dag(const Hash& hash = Hash(),
                 const KeyEqual& eq = KeyEqual())
        : key_to_id_(0, hash, eq),
          id_to_key_(),
          out_edges_(),
          in_edges_(),
          edge_count_(0) {}

    Dag(const Dag&) = default;

    Dag& operator=(const Dag& other) {
        if (this != &other) {
            Dag tmp(other);
            swap(tmp);
        }
        return *this;
    }

    Dag(Dag&& other)
        : key_to_id_(std::move(other.key_to_id_)),
          id_to_key_(std::move(other.id_to_key_)),
          out_edges_(std::move(other.out_edges_)),
          in_edges_(std::move(other.in_edges_)),
          edge_count_(other.edge_count_) {
        other.clear();
    }

    Dag& operator=(Dag&& other) {
        if (this != &other) {
            key_to_id_ = std::move(other.key_to_id_);
            id_to_key_ = std::move(other.id_to_key_);
            out_edges_ = std::move(other.out_edges_);
            in_edges_ = std::move(other.in_edges_);
            edge_count_ = other.edge_count_;
            other.clear();
        }
        return *this;
    }

    void swap(Dag& other) {
        key_to_id_.swap(other.key_to_id_);
        id_to_key_.swap(other.id_to_key_);
        out_edges_.swap(other.out_edges_);
        in_edges_.swap(other.in_edges_);
        std::swap(edge_count_, other.edge_count_);
    }

    void reserve(std::size_t expected_nodes) {
        if (expected_nodes > static_cast<std::size_t>(kInvalidNode)) {
            throw std::length_error("Dag node count exceeds NodeId capacity");
        }
        key_to_id_.reserve(expected_nodes);
        id_to_key_.reserve(expected_nodes);
        out_edges_.reserve(expected_nodes);
        in_edges_.reserve(expected_nodes);
    }

    // Registers an isolated node if not already present.
    // Returns true if inserted, false if already present.
    // Strong exception guarantee: on allocation failure, state is unchanged.
    bool add_node(const Key& key) {
        if (key_to_id_.find(key) != key_to_id_.end()) {
            return false;
        }
        if (id_to_key_.size() >= static_cast<std::size_t>(kInvalidNode)) {
            throw std::length_error("Dag node count exceeds NodeId capacity");
        }
        Key key_copy(key);
        const std::size_t target_n = id_to_key_.size() + 1;
        reserve_map_for(target_n);
        detail::reserve_geometric(id_to_key_, target_n);
        detail::reserve_geometric(out_edges_, target_n);
        detail::reserve_geometric(in_edges_, target_n);

        const NodeId id = static_cast<NodeId>(id_to_key_.size());
        std::pair<typename MapType::iterator, bool> ins =
            key_to_id_.emplace(key_copy, id);
        try {
            id_to_key_.push_back(std::move(key_copy));
            out_edges_.emplace_back();
            in_edges_.emplace_back();
        } catch (...) {
            key_to_id_.erase(ins.first);
            if (id_to_key_.size() > static_cast<std::size_t>(id)) {
                id_to_key_.pop_back();
            }
            if (out_edges_.size() > static_cast<std::size_t>(id)) {
                out_edges_.pop_back();
            }
            if (in_edges_.size() > static_cast<std::size_t>(id)) {
                in_edges_.pop_back();
            }
            throw;
        }
        return true;
    }

    // Adds directed edge `from -> to` (`from` must execute before `to`).
    // Automatically registers `from` and `to` if missing.
    // Safe even if `from` or `to` aliases an element of `nodes()`.
    // Returns true if newly added, false if the edge already existed.
    // Strong exception guarantee: on allocation failure, newly added nodes/edges
    // are rolled back completely without iterator invalidation.
    bool add_edge(const Key& from, const Key& to) {
        typename MapType::const_iterator uit = key_to_id_.find(from);
        typename MapType::const_iterator vit = key_to_id_.find(to);

        const bool from_exists = (uit != key_to_id_.end());
        const bool to_exists = (vit != key_to_id_.end());
        const bool same_missing =
            (!from_exists && !to_exists && key_to_id_.key_eq()(from, to));

        // Snapshot missing keys BEFORE any container mutation so aliased references
        // (e.g. `dag.add_edge(new_key, dag.nodes()[0])`) can never dangle.
        std::unique_ptr<Key> from_copy;
        std::unique_ptr<Key> to_copy;
        if (!from_exists) {
            from_copy.reset(new Key(from));
        }
        if (!to_exists && !same_missing) {
            to_copy.reset(new Key(to));
        }

        if (from_exists && to_exists) {
            const NodeId u = uit->second;
            const NodeId v = vit->second;
            std::vector<NodeId>& outs = out_edges_[u];
            typename std::vector<NodeId>::iterator out_it =
                std::lower_bound(outs.begin(), outs.end(), v);
            if (out_it != outs.end() && *out_it == v) {
                return false;
            }
            std::vector<NodeId>& ins = in_edges_[v];
            typename std::vector<NodeId>::iterator in_it =
                std::lower_bound(ins.begin(), ins.end(), u);

            const std::size_t out_idx = static_cast<std::size_t>(out_it - outs.begin());
            const std::size_t in_idx = static_cast<std::size_t>(in_it - ins.begin());
            detail::reserve_geometric(outs, outs.size() + 1);
            detail::reserve_geometric(ins, ins.size() + 1);

            outs.insert(outs.begin() + static_cast<std::ptrdiff_t>(out_idx), v);
            ins.insert(ins.begin() + static_cast<std::ptrdiff_t>(in_idx), u);
            ++edge_count_;
            return true;
        }

        const std::size_t new_nodes =
            (from_exists ? 0 : 1) + ((to_exists || same_missing) ? 0 : 1);
        const std::size_t old_n = id_to_key_.size();
        if (new_nodes > static_cast<std::size_t>(kInvalidNode) - old_n) {
            throw std::length_error("Dag node count exceeds NodeId capacity");
        }
        const std::size_t target_n = old_n + new_nodes;

        // Reserve map & node arrays BEFORE inserting either node so the second
        // map insertion cannot rehash and invalidate `from_it`.
        NodeId u = from_exists ? uit->second : kInvalidNode;
        NodeId v = to_exists ? vit->second : kInvalidNode;
        reserve_map_for(target_n);
        detail::reserve_geometric(id_to_key_, target_n);
        detail::reserve_geometric(out_edges_, target_n);
        detail::reserve_geometric(in_edges_, target_n);

        // Pre-reserve existing endpoint adjacency vector before adding any node.
        if (from_exists) {
            detail::reserve_geometric(out_edges_[u], out_edges_[u].size() + 1);
        }
        if (to_exists) {
            detail::reserve_geometric(in_edges_[v], in_edges_[v].size() + 1);
        }

        bool added_from = false;
        bool added_to = false;
        typename MapType::iterator from_it;
        typename MapType::iterator to_it;

        try {
            if (!from_exists) {
                u = static_cast<NodeId>(id_to_key_.size());
                std::pair<typename MapType::iterator, bool> ins =
                    key_to_id_.emplace(*from_copy, u);
                from_it = ins.first;
                try {
                    id_to_key_.push_back(std::move(*from_copy));
                    out_edges_.emplace_back();
                    in_edges_.emplace_back();
                } catch (...) {
                    key_to_id_.erase(from_it);
                    if (id_to_key_.size() > static_cast<std::size_t>(u)) {
                        id_to_key_.pop_back();
                    }
                    if (out_edges_.size() > static_cast<std::size_t>(u)) {
                        out_edges_.pop_back();
                    }
                    if (in_edges_.size() > static_cast<std::size_t>(u)) {
                        in_edges_.pop_back();
                    }
                    throw;
                }
                added_from = true;
            }

            if (same_missing) {
                v = u;
            } else if (!to_exists) {
                v = static_cast<NodeId>(id_to_key_.size());
                std::pair<typename MapType::iterator, bool> ins =
                    key_to_id_.emplace(*to_copy, v);
                to_it = ins.first;
                try {
                    id_to_key_.push_back(std::move(*to_copy));
                    out_edges_.emplace_back();
                    in_edges_.emplace_back();
                } catch (...) {
                    key_to_id_.erase(to_it);
                    if (id_to_key_.size() > static_cast<std::size_t>(v)) {
                        id_to_key_.pop_back();
                    }
                    if (out_edges_.size() > static_cast<std::size_t>(v)) {
                        out_edges_.pop_back();
                    }
                    if (in_edges_.size() > static_cast<std::size_t>(v)) {
                        in_edges_.pop_back();
                    }
                    throw;
                }
                added_to = true;
            }

            std::vector<NodeId>& outs = out_edges_[u];
            std::vector<NodeId>& ins = in_edges_[v];
            detail::reserve_geometric(outs, outs.size() + 1);
            detail::reserve_geometric(ins, ins.size() + 1);

            typename std::vector<NodeId>::iterator out_it =
                std::lower_bound(outs.begin(), outs.end(), v);
            typename std::vector<NodeId>::iterator in_it =
                std::lower_bound(ins.begin(), ins.end(), u);

            outs.insert(out_it, v);
            ins.insert(in_it, u);
            ++edge_count_;
            return true;
        } catch (...) {
            if (added_to) {
                rollback_last_node(to_it);
            }
            if (added_from) {
                rollback_last_node(from_it);
            }
            throw;
        }
    }

    // Convenience alias: `node` depends on `depends_on` (equivalent to add_edge(depends_on, node)).
    bool add_dependency(const Key& node, const Key& depends_on) {
        return add_edge(depends_on, node);
    }

    // Removes directed edge `from -> to` if present. Returns true if removed.
    bool remove_edge(const Key& from, const Key& to) {
        typename MapType::const_iterator uit = key_to_id_.find(from);
        if (uit == key_to_id_.end()) {
            return false;
        }
        typename MapType::const_iterator vit = key_to_id_.find(to);
        if (vit == key_to_id_.end()) {
            return false;
        }
        const NodeId u = uit->second;
        const NodeId v = vit->second;

        std::vector<NodeId>& outs = out_edges_[u];
        typename std::vector<NodeId>::iterator out_it =
            std::lower_bound(outs.begin(), outs.end(), v);
        if (out_it == outs.end() || *out_it != v) {
            return false;
        }
        outs.erase(out_it);

        std::vector<NodeId>& ins = in_edges_[v];
        typename std::vector<NodeId>::iterator in_it =
            std::lower_bound(ins.begin(), ins.end(), u);
        if (in_it != ins.end() && *in_it == u) {
            ins.erase(in_it);
        }
        --edge_count_;
        return true;
    }

    void clear() noexcept {
        key_to_id_.clear();
        id_to_key_.clear();
        out_edges_.clear();
        in_edges_.clear();
        edge_count_ = 0;
    }

    // -------------------------------------------------------------------------
    // Queries
    // -------------------------------------------------------------------------

    std::size_t num_nodes() const noexcept { return id_to_key_.size(); }
    std::size_t size() const noexcept { return id_to_key_.size(); }
    std::size_t num_edges() const noexcept { return edge_count_; }
    bool empty() const noexcept { return id_to_key_.empty(); }

    bool contains(const Key& key) const {
        return key_to_id_.find(key) != key_to_id_.end();
    }

    bool has_edge(const Key& from, const Key& to) const {
        typename MapType::const_iterator uit = key_to_id_.find(from);
        if (uit == key_to_id_.end()) {
            return false;
        }
        typename MapType::const_iterator vit = key_to_id_.find(to);
        if (vit == key_to_id_.end()) {
            return false;
        }
        const std::vector<NodeId>& outs = out_edges_[uit->second];
        return std::binary_search(outs.begin(), outs.end(), vit->second);
    }

    const std::vector<Key>& nodes() const noexcept {
        return id_to_key_;
    }

    std::size_t in_degree(const Key& node) const {
        return in_edges_[require_node(node)].size();
    }

    std::size_t out_degree(const Key& node) const {
        return out_edges_[require_node(node)].size();
    }

    // Direct prerequisites of `node` (all `p` such that `p -> node`).
    std::vector<Key> prerequisites(const Key& node) const {
        const NodeId id = require_node(node);
        const std::vector<NodeId>& ins = in_edges_[id];
        std::vector<Key> out;
        out.reserve(ins.size());
        for (std::size_t i = 0; i < ins.size(); ++i) {
            out.push_back(id_to_key_[ins[i]]);
        }
        return out;
    }

    // Direct dependents of `node` (all `d` such that `node -> d`).
    std::vector<Key> dependents(const Key& node) const {
        const NodeId id = require_node(node);
        const std::vector<NodeId>& outs = out_edges_[id];
        std::vector<Key> out;
        out.reserve(outs.size());
        for (std::size_t i = 0; i < outs.size(); ++i) {
            out.push_back(id_to_key_[outs[i]]);
        }
        return out;
    }

    // All transitive upstream prerequisites of `node` (excluding `node` itself),
    // discovered in iterative BFS order.
    std::vector<Key> ancestors(const Key& node) const {
        return collect_reachable(require_node(node), in_edges_);
    }

    // All transitive downstream dependents of `node` (excluding `node` itself),
    // discovered in iterative BFS order.
    std::vector<Key> descendants(const Key& node) const {
        return collect_reachable(require_node(node), out_edges_);
    }

    // -------------------------------------------------------------------------
    // Cycle detection & topological sorting (100% iterative)
    // -------------------------------------------------------------------------

    // Iterative 3-color DFS cycle detection.
    // If a cycle exists, fills `cycle_out` with [c0, c1, ..., ck, c0] and returns true.
    // Otherwise clears `cycle_out` and returns false.
    bool find_cycle(std::vector<Key>& cycle_out) const {
        const std::size_t n = id_to_key_.size();
        if (n == 0) {
            cycle_out.clear();
            return false;
        }

        // 0 = unvisited, 1 = on current DFS stack, 2 = completely explored
        std::vector<std::uint8_t> color(n, 0);
        static const std::size_t kNotOnStack = std::numeric_limits<std::size_t>::max();
        std::vector<std::size_t> stack_pos(n, kNotOnStack);

        struct Frame {
            NodeId node;
            std::size_t edge_idx;
        };
        std::vector<Frame> stack;

        for (std::size_t start = 0; start < n; ++start) {
            if (color[start] != 0) {
                continue;
            }
            const NodeId start_id = static_cast<NodeId>(start);
            color[start_id] = 1;
            stack_pos[start_id] = stack.size();
            stack.push_back(Frame{start_id, 0});

            while (!stack.empty()) {
                Frame& top = stack.back();
                const std::vector<NodeId>& outs = out_edges_[top.node];
                if (top.edge_idx >= outs.size()) {
                    color[top.node] = 2;
                    stack_pos[top.node] = kNotOnStack;
                    stack.pop_back();
                    continue;
                }

                const NodeId nb = outs[top.edge_idx++];
                if (color[nb] == 1) {
                    const std::size_t cycle_start = stack_pos[nb];
                    std::vector<Key> cycle;
                    cycle.reserve((stack.size() - cycle_start) + 1);
                    for (std::size_t i = cycle_start; i < stack.size(); ++i) {
                        cycle.push_back(id_to_key_[stack[i].node]);
                    }
                    cycle.push_back(id_to_key_[nb]);
                    cycle_out.swap(cycle);
                    return true;
                }
                if (color[nb] == 0) {
                    color[nb] = 1;
                    stack_pos[nb] = stack.size();
                    stack.push_back(Frame{nb, 0});
                }
            }
        }

        cycle_out.clear();
        return false;
    }

    // Fast O(V + E) cycle check using Kahn's in-degree counting (zero Key copies).
    bool has_cycle() const {
        std::vector<NodeId> ignored_order;
        std::size_t ignored_width = 0;
        return !analyze_kahn(ignored_order, ignored_width, false);
    }

    // Deterministic Kahn topological sort (ties broken by node insertion order).
    // Returns true and writes all keys to `order_out` if acyclic; clears `order_out`
    // and returns false if the graph contains a cycle.
    bool topological_sort(std::vector<Key>& order_out) const {
        const std::size_t n = id_to_key_.size();
        if (n == 0) {
            order_out.clear();
            return true;
        }

        std::vector<std::size_t> in_deg(n);
        std::priority_queue<NodeId, std::vector<NodeId>, std::greater<NodeId> > ready;
        for (std::size_t i = 0; i < n; ++i) {
            in_deg[i] = in_edges_[i].size();
            if (in_deg[i] == 0) {
                ready.push(static_cast<NodeId>(i));
            }
        }

        std::vector<Key> result;
        result.reserve(n);
        while (!ready.empty()) {
            const NodeId u = ready.top();
            ready.pop();
            result.push_back(id_to_key_[u]);

            const std::vector<NodeId>& outs = out_edges_[u];
            for (std::size_t i = 0; i < outs.size(); ++i) {
                const NodeId v = outs[i];
                if (--in_deg[v] == 0) {
                    ready.push(v);
                }
            }
        }

        if (result.size() != n) {
            order_out.clear();
            return false;
        }
        order_out.swap(result);
        return true;
    }

    // Deterministic layered Kahn BFS topological sort.
    // Layer 0 holds all root nodes (in-degree 0). Every node in layer k depends
    // only on nodes in layers < k, so nodes within the same layer can run in parallel.
    // Returns false and clears `layers_out` if cyclic.
    bool topological_layers(std::vector<std::vector<Key> >& layers_out) const {
        const std::size_t n = id_to_key_.size();
        if (n == 0) {
            layers_out.clear();
            return true;
        }

        std::vector<std::size_t> in_deg(n);
        std::vector<NodeId> current;
        for (std::size_t i = 0; i < n; ++i) {
            in_deg[i] = in_edges_[i].size();
            if (in_deg[i] == 0) {
                current.push_back(static_cast<NodeId>(i));
            }
        }

        std::vector<std::vector<Key> > layers;
        std::vector<NodeId> next;
        std::size_t placed = 0;

        while (!current.empty()) {
            layers.emplace_back();
            std::vector<Key>& layer = layers.back();
            layer.reserve(current.size());

            for (std::size_t i = 0; i < current.size(); ++i) {
                const NodeId u = current[i];
                layer.push_back(id_to_key_[u]);
                ++placed;

                const std::vector<NodeId>& outs = out_edges_[u];
                for (std::size_t j = 0; j < outs.size(); ++j) {
                    const NodeId v = outs[j];
                    if (--in_deg[v] == 0) {
                        next.push_back(v);
                    }
                }
            }
            std::sort(next.begin(), next.end());
            current.swap(next);
            next.clear();
        }

        if (placed != n) {
            layers_out.clear();
            return false;
        }
        layers_out.swap(layers);
        return true;
    }

    // -------------------------------------------------------------------------
    // Dynamic ready-queue parallel DAG execution
    // -------------------------------------------------------------------------

    // Executes `fn(const Key&)` for every node respecting all dependency edges.
    // A node starts as soon as all its prerequisites finish (no unnecessary
    // barrier wait for unrelated slow branches in the same topological layer).
    //
    // Guarantees:
    //   - Validates acyclicity before running any node (throws std::logic_error if cyclic).
    //   - All scheduling buffers are pre-allocated before starting workers so
    //     worker scheduling paths never allocate or invoke std::terminate.
    //   - Full happens-before visibility from every prerequisite's `fn` to its dependents' `fn`.
    //   - On any exception from `fn`, cancels remaining unstarted nodes, joins all
    //     worker threads via RAII, and rethrows the first captured exception.
    //   - Graceful degradation on thread-spawn failure (caller participates as worker 0).
    template <typename Fn>
    void execute_parallel(std::size_t num_threads, Fn&& fn) const {
        const std::size_t n = id_to_key_.size();
        if (n == 0) {
            return;
        }

        std::vector<NodeId> topo_order;
        std::size_t max_width = 0;
        if (!analyze_kahn(topo_order, max_width, true)) {
            throw std::logic_error("Dag::execute_parallel cannot run on a cyclic graph");
        }

        // When every Kahn layer has width 1 the graph is a strict total order
        // (max antichain == 1), so serial execution avoids spawning idle threads.
        // Otherwise allow up to min(num_threads, n) because dynamic cross-layer
        // antichain parallelism can exceed the maximum single Kahn layer width.
        const std::size_t max_parallel = (max_width <= 1) ? 1 : n;
        const std::size_t nworkers = detail::resolve_worker_count(num_threads, max_parallel);
        if (nworkers == 1) {
            for (std::size_t i = 0; i < topo_order.size(); ++i) {
                fn(id_to_key_[topo_order[i]]);
            }
            return;
        }

        // Pre-allocate fixed-capacity ready queue (each node enters at most once),
        // making worker enqueueing strictly allocation-free.
        std::vector<std::size_t> remaining_deps(n);
        std::vector<NodeId> ready;
        ready.reserve(n);
        std::size_t ready_head = 0;
        for (std::size_t i = 0; i < n; ++i) {
            remaining_deps[i] = in_edges_[i].size();
            if (remaining_deps[i] == 0) {
                ready.push_back(static_cast<NodeId>(i));
            }
        }

        std::mutex mu;
        std::condition_variable cv;
        std::size_t completed = 0;
        bool cancelled = false;
        std::exception_ptr first_err;

        auto worker = [&]() noexcept {
            for (;;) {
                NodeId u = kInvalidNode;
                {
                    std::unique_lock<std::mutex> lk(mu);
                    cv.wait(lk, [&]() {
                        return ready_head < ready.size() || cancelled || completed == n;
                    });
                    if (cancelled || completed == n) {
                        return;
                    }
                    u = ready[ready_head++];
                }

                try {
                    fn(id_to_key_[u]);
                } catch (...) {
                    std::lock_guard<std::mutex> lk(mu);
                    cancelled = true;
                    if (!first_err) {
                        first_err = std::current_exception();
                    }
                    cv.notify_all();
                    return;
                }

                {
                    std::lock_guard<std::mutex> lk(mu);
                    ++completed;
                    if (cancelled) {
                        return;
                    }
                    std::size_t unlocked = 0;
                    const std::vector<NodeId>& outs = out_edges_[u];
                    for (std::size_t i = 0; i < outs.size(); ++i) {
                        const NodeId v = outs[i];
                        if (--remaining_deps[v] == 0) {
                            ready.push_back(v);
                            ++unlocked;
                        }
                    }
                    if (completed == n || unlocked > 1) {
                        cv.notify_all();
                    } else if (unlocked == 1) {
                        cv.notify_one();
                    }
                }
            }
        };

        {
            std::vector<std::thread> threads;
            threads.reserve(nworkers - 1);
            detail::ThreadJoiner joiner(threads);

            for (std::size_t w = 1; w < nworkers; ++w) {
                try {
                    threads.emplace_back(worker);
                } catch (...) {
                    break; // Caller drains remaining work inline.
                }
            }
            worker();
        } // All spawned threads are guaranteed joined here.

        if (first_err) {
            std::rethrow_exception(first_err);
        }
    }

    // Executes the DAG using an external thread pool (e.g. myframe::TaskExecutor)
    // without spawning OS threads per call.
    //
    // `submit_fn(std::function<void()>) -> bool` must be non-blocking (return false
    // when queue full or shutting down rather than blocking the caller). Calls to
    // `submit_fn` are serialized internally via a non-blocking try_lock mutex so
    // non-thread-safe adapters remain race-free and synchronous inline adapters
    // cannot recursively deadlock.
    //
    // Deadlock & parallelism guarantees:
    //   - Pool tasks NEVER block waiting on condition variables or other tasks; each
    //     pool task drains immediately ready nodes and dispatches additional helpers
    //     when unlocking wide fan-out branches (preventing parallelism collapse even
    //     while the caller is busy executing a slow node).
    //   - The caller thread executes ready nodes inline and waits ONLY while at least
    //     one node is actively running (`in_flight > 0`) on a live OS thread.
    //   - Precondition: `fn` must NOT block waiting on another task in the same pool,
    //     `submit_fn` must NOT block waiting for pool space, and caller must NOT hold
    //     a mutex required by pool workers.
    template <typename SubmitFn, typename Fn>
    void execute_parallel_with_executor(std::size_t max_workers,
                                        SubmitFn&& submit_fn,
                                        Fn&& fn) const {
        const std::size_t n = id_to_key_.size();
        if (n == 0) {
            return;
        }

        std::vector<NodeId> topo_order;
        std::size_t max_width = 0;
        if (!analyze_kahn(topo_order, max_width, true)) {
            throw std::logic_error(
                "Dag::execute_parallel_with_executor cannot run on a cyclic graph");
        }

        const std::size_t max_parallel = (max_width <= 1) ? 1 : n;
        const std::size_t nworkers = detail::resolve_worker_count(max_workers, max_parallel);
        if (nworkers == 1) {
            for (std::size_t i = 0; i < topo_order.size(); ++i) {
                fn(id_to_key_[topo_order[i]]);
            }
            return;
        }

        typedef typename std::remove_reference<SubmitFn>::type SubmitFnT;
        typedef typename std::remove_reference<Fn>::type FnT;

        struct SubmitTicket {
            bool released;
            SubmitTicket() noexcept : released(false) {}
        };

        struct SharedState {
            const Dag* dag;
            SubmitFnT* submit_ptr;
            FnT* fn_ptr;
            std::size_t max_helpers;
            std::vector<std::size_t> remaining_deps;
            std::vector<NodeId> ready;
            std::size_t ready_head;
            std::size_t total_nodes;
            std::size_t completed;
            std::size_t in_flight;
            std::size_t active_pool_tasks;
            std::size_t outstanding_submits;
            bool cancelled;
            bool closed;
            std::exception_ptr first_err;
            std::mutex mu;
            std::mutex submit_mu;
            std::condition_variable cv;

            SharedState(const Dag* d,
                        SubmitFnT* sub,
                        FnT* f,
                        std::size_t total,
                        std::size_t helpers_cap)
                : dag(d),
                  submit_ptr(sub),
                  fn_ptr(f),
                  max_helpers(helpers_cap),
                  remaining_deps(total),
                  ready(),
                  ready_head(0),
                  total_nodes(total),
                  completed(0),
                  in_flight(0),
                  active_pool_tasks(0),
                  outstanding_submits(0),
                  cancelled(false),
                  closed(false),
                  first_err(),
                  mu(),
                  submit_mu(),
                  cv() {
                ready.reserve(total);
                for (std::size_t i = 0; i < total; ++i) {
                    remaining_deps[i] = d->in_edges_[i].size();
                    if (remaining_deps[i] == 0) {
                        ready.push_back(static_cast<NodeId>(i));
                    }
                }
            }

            std::size_t ready_count_locked() const noexcept {
                return ready.size() - ready_head;
            }

            void release_ticket_locked(SubmitTicket& ticket) noexcept {
                if (!ticket.released) {
                    ticket.released = true;
                    if (outstanding_submits > 0) {
                        --outstanding_submits;
                    }
                }
            }
        };

        std::shared_ptr<SharedState> st(
            new SharedState(this, std::addressof(submit_fn), std::addressof(fn),
                            n, nworkers - 1));

        // RAII guard guarantees `closed = true` and `active_pool_tasks == 0` wait
        // ALWAYS complete before caller stack unwinding destroys `fn`, `submit_fn`, or `this`.
        struct CloseAndWaitGuard {
            std::shared_ptr<SharedState> state;
            bool done;
            explicit CloseAndWaitGuard(std::shared_ptr<SharedState> s) noexcept
                : state(std::move(s)), done(false) {}

            void close_and_extract(std::exception_ptr& out_err, bool mark_cancelled) {
                if (done) {
                    return;
                }
                std::unique_lock<std::mutex> lk(state->mu);
                if (mark_cancelled) {
                    state->cancelled = true;
                }
                state->closed = true;
                state->cv.notify_all();
                state->cv.wait(lk, [this]() { return state->active_pool_tasks == 0; });
                out_err = state->first_err;
                state->first_err = std::exception_ptr();
                done = true;
            }

            ~CloseAndWaitGuard() noexcept {
                if (!done) {
                    std::exception_ptr discarded;
                    close_and_extract(discarded, true);
                }
            }
        } close_guard(st);

        // Static-style helper struct avoids self-referential lambda capture overhead.
        struct Runner {
            static void execute_one(SharedState& s, NodeId u) noexcept {
                try {
                    (*s.fn_ptr)(s.dag->id_to_key_[u]);
                } catch (...) {
                    std::lock_guard<std::mutex> lk(s.mu);
                    --s.in_flight;
                    s.cancelled = true;
                    if (!s.first_err) {
                        s.first_err = std::current_exception();
                    }
                    s.cv.notify_all();
                    return;
                }

                std::lock_guard<std::mutex> lk(s.mu);
                --s.in_flight;
                ++s.completed;
                if (!s.cancelled) {
                    const std::vector<NodeId>& outs = s.dag->out_edges_[u];
                    for (std::size_t i = 0; i < outs.size(); ++i) {
                        const NodeId v = outs[i];
                        if (--s.remaining_deps[v] == 0) {
                            s.ready.push_back(v);
                        }
                    }
                }
                s.cv.notify_all();
            }

            static void try_dispatch_helpers(const std::shared_ptr<SharedState>& s,
                                             std::size_t min_ready_needed) noexcept {
                // Non-blocking try_lock serializes `submit_fn` across caller & active
                // pool tasks without ever deadlocking synchronous inline executors.
                std::unique_lock<std::mutex> slk(s->submit_mu, std::try_to_lock);
                if (!slk.owns_lock()) {
                    return;
                }

                for (;;) {
                    std::shared_ptr<SubmitTicket> ticket;
                    try {
                        ticket.reset(new SubmitTicket());
                    } catch (...) {
                        return;
                    }

                    {
                        std::lock_guard<std::mutex> lk(s->mu);
                        const std::size_t rcnt = s->ready_count_locked();
                        const std::size_t queued_unstarted =
                            (s->outstanding_submits > s->active_pool_tasks)
                                ? (s->outstanding_submits - s->active_pool_tasks)
                                : 0;
                        const std::size_t needed_helpers =
                            (rcnt >= min_ready_needed)
                                ? (rcnt - (min_ready_needed - 1))
                                : 0;
                        if (s->cancelled || s->closed ||
                            s->outstanding_submits >= s->max_helpers ||
                            needed_helpers == 0 ||
                            queued_unstarted >= needed_helpers) {
                            return;
                        }
                        ++s->outstanding_submits;
                    }

                    bool accepted = false;
                    try {
                        std::shared_ptr<SharedState> cap = s;
                        std::shared_ptr<SubmitTicket> tcap = ticket;
                        std::function<void()> task([cap, tcap]() {
                            {
                                std::lock_guard<std::mutex> lk(cap->mu);
                                if (cap->closed || cap->cancelled ||
                                    cap->completed == cap->total_nodes) {
                                    cap->release_ticket_locked(*tcap);
                                    return;
                                }
                                ++cap->active_pool_tasks;
                            }

                            for (;;) {
                                NodeId u = kInvalidNode;
                                bool want_fanout = false;
                                {
                                    std::lock_guard<std::mutex> lk(cap->mu);
                                    if (cap->closed || cap->cancelled ||
                                        cap->ready_count_locked() == 0) {
                                        break;
                                    }
                                    u = cap->ready[cap->ready_head++];
                                    ++cap->in_flight;
                                    want_fanout =
                                        (cap->ready_count_locked() >= 1 &&
                                         cap->outstanding_submits < cap->max_helpers);
                                }
                                if (want_fanout) {
                                    // Fan out newly unlocked parallel siblings even if
                                    // the caller thread is busy executing a slow node.
                                    try_dispatch_helpers(cap, 1);
                                }
                                execute_one(*cap, u);
                            }

                            {
                                std::lock_guard<std::mutex> lk(cap->mu);
                                --cap->active_pool_tasks;
                                cap->release_ticket_locked(*tcap);
                                if (cap->active_pool_tasks == 0 || cap->in_flight == 0) {
                                    cap->cv.notify_all();
                                }
                            }
                        });
                        accepted = (*s->submit_ptr)(std::move(task));
                    } catch (...) {
                        accepted = false;
                    }

                    if (!accepted) {
                        std::lock_guard<std::mutex> lk(s->mu);
                        s->release_ticket_locked(*ticket);
                        return;
                    }
                }
            }
        };

        // Caller thread drives scheduling and participates inline.
        for (;;) {
            NodeId u = kInvalidNode;
            bool need_dispatch = false;
            {
                std::unique_lock<std::mutex> lk(st->mu);
                for (;;) {
                    if (st->cancelled || st->completed == n) {
                        break;
                    }
                    if (st->ready_count_locked() > 0) {
                        need_dispatch =
                            (st->ready_count_locked() > 1 &&
                             st->outstanding_submits < st->max_helpers);
                        break;
                    }
                    // Ready is empty, not cancelled, not complete => an active pool worker
                    // MUST be running a prerequisite (`in_flight > 0`).
                    if (st->in_flight == 0) {
                        break;
                    }
                    st->cv.wait(lk, [&]() {
                        return st->ready_count_locked() > 0 || st->cancelled ||
                               st->completed == n || st->in_flight == 0;
                    });
                }

                if (st->cancelled || st->completed == n) {
                    break;
                }
            }

            if (need_dispatch) {
                Runner::try_dispatch_helpers(st, 2);
            }

            {
                std::lock_guard<std::mutex> lk(st->mu);
                if (st->cancelled || st->completed == n) {
                    break;
                }
                if (st->ready_count_locked() == 0) {
                    continue;
                }
                u = st->ready[st->ready_head++];
                ++st->in_flight;
            }

            Runner::execute_one(*st, u);
        }

        std::exception_ptr err;
        close_guard.close_and_extract(err, false);
        if (err) {
            std::rethrow_exception(err);
        }
    }

private:
    typedef std::unordered_map<Key, NodeId, Hash, KeyEqual> MapType;
    static const NodeId kInvalidNode = std::numeric_limits<NodeId>::max();

    void reserve_map_for(std::size_t target_elements) {
        const std::size_t buckets = key_to_id_.bucket_count();
        const double max_before_rehash =
            static_cast<double>(buckets) * static_cast<double>(key_to_id_.max_load_factor());
        if (buckets > 1 && static_cast<double>(target_elements) < max_before_rehash) {
            return;
        }
        // Grow geometrically when rehash is needed so repeated single insertions remain O(1) amortized.
        std::size_t grow_to = (key_to_id_.size() < 4) ? std::size_t(8) : (key_to_id_.size() * 2);
        if (grow_to < target_elements) {
            grow_to = target_elements;
        }
        if (grow_to > static_cast<std::size_t>(kInvalidNode)) {
            grow_to = target_elements;
        }
        key_to_id_.reserve(grow_to);
    }

    void rollback_last_node(typename MapType::iterator map_it) noexcept {
        key_to_id_.erase(map_it);
        id_to_key_.pop_back();
        out_edges_.pop_back();
        in_edges_.pop_back();
    }

    NodeId require_node(const Key& key) const {
        typename MapType::const_iterator it = key_to_id_.find(key);
        if (it == key_to_id_.end()) {
            throw std::out_of_range("Dag node not found");
        }
        return it->second;
    }

    std::vector<Key> collect_reachable(
        NodeId start,
        const std::vector<std::vector<NodeId> >& adj) const {
        const std::size_t n = id_to_key_.size();
        std::vector<std::uint8_t> visited(n, 0);
        std::vector<NodeId> queue;
        visited[start] = 1;
        queue.push_back(start);

        std::vector<Key> out;
        std::size_t head = 0;
        while (head < queue.size()) {
            const NodeId u = queue[head++];
            const std::vector<NodeId>& nbs = adj[u];
            for (std::size_t i = 0; i < nbs.size(); ++i) {
                const NodeId v = nbs[i];
                if (!visited[v]) {
                    visited[v] = 1;
                    queue.push_back(v);
                    out.push_back(id_to_key_[v]);
                }
            }
        }
        return out;
    }

    // Single-pass layered Kahn check computing acyclicity, max layer width, and
    // optionally a deterministic topological order of NodeIds.
    bool analyze_kahn(std::vector<NodeId>& order_out,
                      std::size_t& max_width_out,
                      bool collect_order) const {
        const std::size_t n = id_to_key_.size();
        max_width_out = 0;
        if (collect_order) {
            order_out.clear();
            order_out.reserve(n);
        }
        if (n == 0) {
            return true;
        }

        std::vector<std::size_t> in_deg(n);
        std::vector<NodeId> current;
        for (std::size_t i = 0; i < n; ++i) {
            in_deg[i] = in_edges_[i].size();
            if (in_deg[i] == 0) {
                current.push_back(static_cast<NodeId>(i));
            }
        }

        std::vector<NodeId> next;
        std::size_t placed = 0;
        while (!current.empty()) {
            if (current.size() > max_width_out) {
                max_width_out = current.size();
            }
            for (std::size_t i = 0; i < current.size(); ++i) {
                const NodeId u = current[i];
                ++placed;
                if (collect_order) {
                    order_out.push_back(u);
                }
                const std::vector<NodeId>& outs = out_edges_[u];
                for (std::size_t j = 0; j < outs.size(); ++j) {
                    const NodeId v = outs[j];
                    if (--in_deg[v] == 0) {
                        next.push_back(v);
                    }
                }
            }
            if (collect_order) {
                std::sort(next.begin(), next.end());
            }
            current.swap(next);
            next.clear();
        }

        if (placed != n) {
            order_out.clear();
            max_width_out = 0;
            return false;
        }
        return true;
    }

    MapType key_to_id_;
    std::vector<Key> id_to_key_;
    std::vector<std::vector<NodeId> > out_edges_;
    std::vector<std::vector<NodeId> > in_edges_;
    std::size_t edge_count_;
};

} // namespace pipeline
} // namespace myframe
