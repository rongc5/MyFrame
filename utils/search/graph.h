#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <queue>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace myframe {
namespace search {

typedef std::uint32_t VertexId;
static const VertexId kInvalidVertex = std::numeric_limits<VertexId>::max();

// Control signal returned by graph traversal visitors.
// Visitors may also return `void` (treated as Continue) or `bool`
// (`true` -> Continue, `false` -> Stop).
//
// Note on SkipBranch: suppresses expanding neighbors of the visited vertex;
// it does not globally exclude descendants reachable via an alternate route.
enum class VisitAction {
    Continue,
    SkipBranch,
    Stop
};

// Traversal bounds for BFS and DFS:
//   - For BFS / multi-source BFS, `max_depth` is the exact shortest-hop radius.
//   - For DFS, `max_depth` is the first-discovery DFS-tree depth (use BFS if
//     you need all vertices within a shortest-hop radius).
struct TraversalOptions {
    std::size_t max_depth;
    std::size_t limit;

    TraversalOptions() noexcept
        : max_depth(std::numeric_limits<std::size_t>::max()),
          limit(std::numeric_limits<std::size_t>::max()) {}
};

// Lightweight read-only contiguous range for CSR adjacency and weight slices.
template <typename T>
class ConstSpan {
public:
    typedef const T* const_iterator;
    typedef const T* iterator;
    typedef T value_type;
    typedef std::size_t size_type;

    ConstSpan() noexcept : data_(nullptr), size_(0) {}
    ConstSpan(const T* data, std::size_t size) noexcept
        : data_(data), size_(size) {}

    const_iterator begin() const noexcept { return data_; }
    const_iterator end() const noexcept { return data_ + size_; }
    const T* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    const T& operator[](std::size_t idx) const noexcept { return data_[idx]; }

private:
    const T* data_;
    std::size_t size_;
};

typedef ConstSpan<VertexId> VertexSpan;

namespace detail {

class CsrCore;

template <typename Fn>
inline typename std::enable_if<
    std::is_same<decltype(std::declval<Fn&>()(VertexId(0), std::size_t(0))), void>::value,
    VisitAction>::type
invoke_traversal_visitor(Fn& fn, VertexId v, std::size_t depth) {
    fn(v, depth);
    return VisitAction::Continue;
}

template <typename Fn>
inline typename std::enable_if<
    std::is_same<decltype(std::declval<Fn&>()(VertexId(0), std::size_t(0))), bool>::value,
    VisitAction>::type
invoke_traversal_visitor(Fn& fn, VertexId v, std::size_t depth) {
    return fn(v, depth) ? VisitAction::Continue : VisitAction::Stop;
}

template <typename Fn>
inline typename std::enable_if<
    std::is_same<decltype(std::declval<Fn&>()(VertexId(0), std::size_t(0))), VisitAction>::value,
    VisitAction>::type
invoke_traversal_visitor(Fn& fn, VertexId v, std::size_t depth) {
    return fn(v, depth);
}

template <typename Weight>
inline typename std::enable_if<std::is_floating_point<Weight>::value, bool>::type
is_finite_weight(Weight w) noexcept {
    return std::isfinite(w);
}

template <typename Weight>
inline typename std::enable_if<!std::is_floating_point<Weight>::value, bool>::type
is_finite_weight(Weight) noexcept {
    return true;
}

template <typename Weight>
inline typename std::enable_if<std::is_signed<Weight>::value, bool>::type
is_negative_weight(Weight w) noexcept {
    return w < Weight(0);
}

template <typename Weight>
inline typename std::enable_if<!std::is_signed<Weight>::value, bool>::type
is_negative_weight(Weight) noexcept {
    return false;
}

enum class AddCutoffStatus {
    Ok,
    ExceedsCutoff,
    Overflow
};

// Adds two non-negative weights with optional cutoff check.
// - Floating-point: compares computed sum directly against `max_val` so exact
//   boundary sums (e.g. 0.9 + 0.1 == 1.0) are never rejected by subtraction rounding.
//   A non-finite positive overflow against a finite cutoff returns ExceedsCutoff.
// - Integral: checks subtraction before adding to guarantee zero overflow/UB.
template <typename Weight>
inline typename std::enable_if<std::is_floating_point<Weight>::value, AddCutoffStatus>::type
add_nonneg_with_cutoff(Weight base,
                       Weight delta,
                       bool has_max,
                       Weight max_val,
                       Weight& out) noexcept {
    const Weight sum = base + delta;
    if (has_max) {
        if (!std::isfinite(sum) || sum > max_val) {
            return AddCutoffStatus::ExceedsCutoff;
        }
        out = sum;
        return AddCutoffStatus::Ok;
    }
    if (!std::isfinite(sum)) {
        return AddCutoffStatus::Overflow;
    }
    out = sum;
    return AddCutoffStatus::Ok;
}

template <typename Weight>
inline typename std::enable_if<!std::is_floating_point<Weight>::value, AddCutoffStatus>::type
add_nonneg_with_cutoff(Weight base,
                       Weight delta,
                       bool has_max,
                       Weight max_val,
                       Weight& out) noexcept {
    if (has_max) {
        if (delta > static_cast<Weight>(max_val - base)) {
            return AddCutoffStatus::ExceedsCutoff;
        }
        out = static_cast<Weight>(base + delta);
        return AddCutoffStatus::Ok;
    }
    const Weight max_lim = std::numeric_limits<Weight>::max();
    if (delta > static_cast<Weight>(max_lim - base)) {
        return AddCutoffStatus::Overflow;
    }
    out = static_cast<Weight>(base + delta);
    return AddCutoffStatus::Ok;
}

// Validates and converts a raw arithmetic heuristic value to Weight BEFORE any
// narrowing or float-to-integer conversion (eliminates UB on NaN/Inf/out-of-range).
template <typename Weight, typename RawH>
inline typename std::enable_if<std::is_same<Weight, RawH>::value, AddCutoffStatus>::type
validate_and_convert_heuristic(RawH raw_h, Weight& out) {
    static_assert(std::is_arithmetic<RawH>::value && !std::is_same<RawH, bool>::value,
                  "A* heuristic must return a non-bool arithmetic type");
    if (!is_finite_weight(raw_h) || is_negative_weight(raw_h)) {
        throw std::invalid_argument(
            "A* heuristic must return finite non-negative values");
    }
    out = raw_h;
    return AddCutoffStatus::Ok;
}

template <typename Weight, typename RawH>
inline typename std::enable_if<
    !std::is_same<Weight, RawH>::value && std::is_floating_point<Weight>::value,
    AddCutoffStatus>::type
validate_and_convert_heuristic(RawH raw_h, Weight& out) {
    static_assert(std::is_arithmetic<RawH>::value && !std::is_same<RawH, bool>::value,
                  "A* heuristic must return a non-bool arithmetic type");
    if (!is_finite_weight(raw_h) || is_negative_weight(raw_h)) {
        throw std::invalid_argument(
            "A* heuristic must return finite non-negative values");
    }
    const Weight conv = static_cast<Weight>(raw_h);
    if (!std::isfinite(conv)) {
        return AddCutoffStatus::Overflow;
    }
    out = conv;
    return AddCutoffStatus::Ok;
}

template <typename Weight, typename RawH>
inline typename std::enable_if<
    !std::is_same<Weight, RawH>::value &&
        std::is_integral<Weight>::value &&
        std::is_integral<RawH>::value,
    AddCutoffStatus>::type
validate_and_convert_heuristic(RawH raw_h, Weight& out) {
    static_assert(std::is_arithmetic<RawH>::value && !std::is_same<RawH, bool>::value,
                  "A* heuristic must return a non-bool arithmetic type");
    if (is_negative_weight(raw_h)) {
        throw std::invalid_argument(
            "A* heuristic must return finite non-negative values");
    }
    const std::uintmax_t mag = static_cast<std::uintmax_t>(raw_h);
    const std::uintmax_t max_w =
        static_cast<std::uintmax_t>(std::numeric_limits<Weight>::max());
    if (mag > max_w) {
        return AddCutoffStatus::Overflow;
    }
    out = static_cast<Weight>(mag);
    return AddCutoffStatus::Ok;
}

template <typename Weight, typename RawH>
inline typename std::enable_if<
    !std::is_same<Weight, RawH>::value &&
        std::is_integral<Weight>::value &&
        std::is_floating_point<RawH>::value,
    AddCutoffStatus>::type
validate_and_convert_heuristic(RawH raw_h, Weight& out) {
    if (!std::isfinite(raw_h) || raw_h < RawH(0)) {
        throw std::invalid_argument(
            "A* heuristic must return finite non-negative values");
    }
    // Exact power-of-two upper bound `2^digits`: any finite value `< bound`
    // truncates strictly inside [0, numeric_limits<Weight>::max()] without UB.
    const long double ld = static_cast<long double>(raw_h);
    const long double upper_exclusive =
        std::ldexp(1.0L, std::numeric_limits<Weight>::digits);
    if (!(ld < upper_exclusive)) {
        return AddCutoffStatus::Overflow;
    }
    out = static_cast<Weight>(ld);
    return AddCutoffStatus::Ok;
}

// Portable strict-C++11 128-bit two's-complement accumulator (`hi * 2^64 + lo`).
// Eliminates intermediate overflow/underflow in Bellman-Ford and Kruskal MSF
// for all standard integral Weight types (up to 64-bit signed/unsigned).
struct Int128 {
    std::int64_t hi;
    std::uint64_t lo;

    Int128() noexcept : hi(0), lo(0) {}
    Int128(std::int64_t h, std::uint64_t l) noexcept : hi(h), lo(l) {}

    template <typename T>
    static typename std::enable_if<
        std::is_integral<T>::value && std::is_signed<T>::value, Int128>::type
    from(T v) noexcept {
        const std::int64_t s = static_cast<std::int64_t>(v);
        return Int128(s < 0 ? std::int64_t(-1) : std::int64_t(0),
                      static_cast<std::uint64_t>(s));
    }

    template <typename T>
    static typename std::enable_if<
        std::is_integral<T>::value && !std::is_signed<T>::value, Int128>::type
    from(T v) noexcept {
        return Int128(0, static_cast<std::uint64_t>(v));
    }

    Int128 operator+(const Int128& rhs) const noexcept {
        const std::uint64_t new_lo = lo + rhs.lo;
        const std::uint64_t carry = (new_lo < lo) ? 1ULL : 0ULL;
        const std::uint64_t new_hi =
            static_cast<std::uint64_t>(hi) +
            static_cast<std::uint64_t>(rhs.hi) + carry;
        return Int128(static_cast<std::int64_t>(new_hi), new_lo);
    }

    bool operator<(const Int128& rhs) const noexcept {
        if (hi != rhs.hi) {
            return hi < rhs.hi;
        }
        return lo < rhs.lo;
    }

    bool operator==(const Int128& rhs) const noexcept {
        return hi == rhs.hi && lo == rhs.lo;
    }

    template <typename T>
    typename std::enable_if<
        std::is_integral<T>::value && std::is_signed<T>::value, bool>::type
    try_to(T& out) const noexcept {
        const Int128 lo_bound = Int128::from(std::numeric_limits<T>::min());
        const Int128 hi_bound = Int128::from(std::numeric_limits<T>::max());
        if (*this < lo_bound || hi_bound < *this) {
            return false;
        }
        const std::int64_t s = static_cast<std::int64_t>(lo);
        out = static_cast<T>(s);
        return true;
    }

    template <typename T>
    typename std::enable_if<
        std::is_integral<T>::value && !std::is_signed<T>::value, bool>::type
    try_to(T& out) const noexcept {
        if (hi != 0) {
            return false;
        }
        const std::uint64_t max_w =
            static_cast<std::uint64_t>(std::numeric_limits<T>::max());
        if (lo > max_w) {
            return false;
        }
        out = static_cast<T>(lo);
        return true;
    }
};

inline void validate_vertex_count(std::size_t num_vertices) {
    if (num_vertices > static_cast<std::size_t>(kInvalidVertex) ||
        num_vertices == std::numeric_limits<std::size_t>::max() ||
        num_vertices + 1 > std::vector<std::size_t>().max_size() ||
        num_vertices > std::vector<VertexId>().max_size()) {
        throw std::length_error("vertex count exceeds VertexId or container capacity");
    }
}

inline void validate_vertex_id(VertexId v, std::size_t num_vertices) {
    if (static_cast<std::size_t>(v) >= num_vertices) {
        throw std::out_of_range("vertex id out of range");
    }
}

} // namespace detail

// Reusable scratch buffers for zero-allocation steady-state BFS / DFS queries
// on large read-only CSR graphs.
//
// Uses a generation-stamped visited array so resetting between traversals is
// O(1) (touching only visited vertices) instead of O(V).
// Not thread-safe: keep one workspace per thread or per query context.
class TraversalWorkspace {
public:
    TraversalWorkspace() noexcept
        : mark_(), generation_(0), bfs_queue_(), dfs_stack_() {}

    void reserve(std::size_t num_vertices, std::size_t queue_hint = 0) {
        detail::validate_vertex_count(num_vertices);
        if (mark_.size() < num_vertices) {
            mark_.resize(num_vertices, 0);
        }
        if (queue_hint > 0) {
            bfs_queue_.reserve(queue_hint);
            dfs_stack_.reserve(queue_hint);
        }
    }

    void clear() noexcept {
        mark_.clear();
        generation_ = 0;
        bfs_queue_.clear();
        dfs_stack_.clear();
    }

    std::size_t vertex_capacity() const noexcept { return mark_.size(); }

private:
    friend class detail::CsrCore;

    struct QueueItem {
        VertexId vertex;
        std::size_t depth;
    };

    struct DfsFrame {
        VertexId vertex;
        std::size_t edge_idx;
        std::size_t depth;
    };

    void prepare(std::size_t num_vertices) {
        if (mark_.size() < num_vertices) {
            mark_.resize(num_vertices, 0);
        }
        if (generation_ == std::numeric_limits<std::uint32_t>::max()) {
            std::fill(mark_.begin(), mark_.end(), std::uint32_t(0));
            generation_ = 0;
        }
        ++generation_;
        bfs_queue_.clear();
        dfs_stack_.clear();
    }

    bool is_visited(VertexId v) const noexcept {
        return mark_[v] == generation_;
    }

    void mark_visited(VertexId v) noexcept {
        mark_[v] = generation_;
    }

    std::vector<std::uint32_t> mark_;
    std::uint32_t generation_;
    std::vector<QueueItem> bfs_queue_;
    std::vector<DfsFrame> dfs_stack_;
};

// Iterative Disjoint Set Union (Union-Find) with path halving and union by size.
//
// Common uses:
//   - Connected-component clustering (anti-fraud device/account graphs)
//   - Incremental connectivity checks across dynamic edge streams
//   - Weakly connected component extraction on CSR graphs
//
// Properties:
//   - Strict C++11/C++17, header-only, iterative (no recursion stack overflow).
//   - Strong exception safety on copy assignment, reset(), and add_vertex().
//   - Noexcept move leaves moved-from object empty and reusable.
class DisjointSet {
public:
    DisjointSet() noexcept : parent_(), size_(), num_components_(0) {}

    explicit DisjointSet(std::size_t num_vertices)
        : parent_(), size_(), num_components_(0) {
        reset(num_vertices);
    }

    DisjointSet(const DisjointSet&) = default;

    DisjointSet& operator=(const DisjointSet& other) {
        if (this != &other) {
            DisjointSet tmp(other);
            *this = std::move(tmp);
        }
        return *this;
    }

    DisjointSet(DisjointSet&& other) noexcept
        : parent_(std::move(other.parent_)),
          size_(std::move(other.size_)),
          num_components_(other.num_components_) {
        other.parent_.clear();
        other.size_.clear();
        other.num_components_ = 0;
    }

    DisjointSet& operator=(DisjointSet&& other) noexcept {
        if (this != &other) {
            parent_ = std::move(other.parent_);
            size_ = std::move(other.size_);
            num_components_ = other.num_components_;
            other.parent_.clear();
            other.size_.clear();
            other.num_components_ = 0;
        }
        return *this;
    }

    void reset(std::size_t num_vertices) {
        detail::validate_vertex_count(num_vertices);
        std::vector<VertexId> new_parent(num_vertices);
        std::vector<VertexId> new_size(num_vertices, 1);
        for (std::size_t i = 0; i < num_vertices; ++i) {
            new_parent[i] = static_cast<VertexId>(i);
        }
        parent_.swap(new_parent);
        size_.swap(new_size);
        num_components_ = num_vertices;
    }

    VertexId add_vertex() {
        detail::validate_vertex_count(parent_.size() + 1);
        const VertexId id = static_cast<VertexId>(parent_.size());
        parent_.push_back(id);
        try {
            size_.push_back(1);
        } catch (...) {
            parent_.pop_back();
            throw;
        }
        ++num_components_;
        return id;
    }

    // Finds root representative of `v` with iterative path halving.
    VertexId find(VertexId v) {
        detail::validate_vertex_id(v, parent_.size());
        while (parent_[v] != v) {
            parent_[v] = parent_[parent_[v]];
            v = parent_[v];
        }
        return v;
    }

    // Read-only find without mutating parent pointers (safe for const / concurrent reads).
    VertexId find_const(VertexId v) const {
        detail::validate_vertex_id(v, parent_.size());
        while (parent_[v] != v) {
            v = parent_[v];
        }
        return v;
    }

    // Unites the sets containing `a` and `b`. Returns true if merged, false if already connected.
    bool unite(VertexId a, VertexId b) {
        detail::validate_vertex_id(a, parent_.size());
        detail::validate_vertex_id(b, parent_.size());
        VertexId ra = find(a);
        VertexId rb = find(b);
        if (ra == rb) {
            return false;
        }
        if (size_[ra] < size_[rb]) {
            std::swap(ra, rb);
        }
        parent_[rb] = ra;
        size_[ra] = static_cast<VertexId>(size_[ra] + size_[rb]);
        --num_components_;
        return true;
    }

    bool connected(VertexId a, VertexId b) {
        detail::validate_vertex_id(a, parent_.size());
        detail::validate_vertex_id(b, parent_.size());
        return find(a) == find(b);
    }

    bool connected_const(VertexId a, VertexId b) const {
        detail::validate_vertex_id(a, parent_.size());
        detail::validate_vertex_id(b, parent_.size());
        return find_const(a) == find_const(b);
    }

    std::size_t component_size(VertexId v) {
        return static_cast<std::size_t>(size_[find(v)]);
    }

    std::size_t component_size_const(VertexId v) const {
        return static_cast<std::size_t>(size_[find_const(v)]);
    }

    // Returns all connected components in deterministic lowest-vertex order.
    std::vector<std::vector<VertexId> > components() const {
        const std::size_t n = parent_.size();
        if (n == 0) {
            return std::vector<std::vector<VertexId> >();
        }
        std::vector<std::size_t> root_to_group(n, std::numeric_limits<std::size_t>::max());
        std::vector<std::vector<VertexId> > groups;
        groups.reserve(num_components_);

        for (std::size_t i = 0; i < n; ++i) {
            const VertexId v = static_cast<VertexId>(i);
            const VertexId r = find_const(v);
            std::size_t gid = root_to_group[r];
            if (gid == std::numeric_limits<std::size_t>::max()) {
                gid = groups.size();
                root_to_group[r] = gid;
                groups.emplace_back();
                groups.back().reserve(size_[r]);
            }
            groups[gid].push_back(v);
        }
        return groups;
    }

    void clear() noexcept {
        parent_.clear();
        size_.clear();
        num_components_ = 0;
    }

    std::size_t num_vertices() const noexcept { return parent_.size(); }
    std::size_t num_components() const noexcept { return num_components_; }
    bool empty() const noexcept { return parent_.empty(); }

private:
    std::vector<VertexId> parent_;
    std::vector<VertexId> size_;
    std::size_t num_components_;
};

namespace detail {

// Shared immutable CSR adjacency storage & iterative traversal core.
//
// Invariants:
//   - num_vertices_ == 0 => offsets_.empty() && targets_.empty()
//   - num_vertices_ > 0  => offsets_.size() == num_vertices_ + 1,
//                           offsets_[0] == 0,
//                           offsets_[num_vertices_] == targets_.size(),
//                           offsets_ is monotonically non-decreasing.
class CsrCore {
public:
    CsrCore() noexcept
        : offsets_(), targets_(), num_vertices_(0), sorted_neighbors_(true) {}

    CsrCore(std::vector<std::size_t>&& offsets,
            std::vector<VertexId>&& targets,
            std::size_t num_vertices,
            bool sorted_neighbors) noexcept
        : offsets_(std::move(offsets)),
          targets_(std::move(targets)),
          num_vertices_(num_vertices),
          sorted_neighbors_(sorted_neighbors) {}

    CsrCore(const CsrCore&) = default;

    CsrCore& operator=(const CsrCore& other) {
        if (this != &other) {
            CsrCore tmp(other);
            *this = std::move(tmp);
        }
        return *this;
    }

    CsrCore(CsrCore&& other) noexcept
        : offsets_(std::move(other.offsets_)),
          targets_(std::move(other.targets_)),
          num_vertices_(other.num_vertices_),
          sorted_neighbors_(other.sorted_neighbors_) {
        other.offsets_.clear();
        other.targets_.clear();
        other.num_vertices_ = 0;
        other.sorted_neighbors_ = true;
    }

    CsrCore& operator=(CsrCore&& other) noexcept {
        if (this != &other) {
            offsets_ = std::move(other.offsets_);
            targets_ = std::move(other.targets_);
            num_vertices_ = other.num_vertices_;
            sorted_neighbors_ = other.sorted_neighbors_;
            other.offsets_.clear();
            other.targets_.clear();
            other.num_vertices_ = 0;
            other.sorted_neighbors_ = true;
        }
        return *this;
    }

    std::size_t num_vertices() const noexcept { return num_vertices_; }
    std::size_t num_edges() const noexcept { return targets_.size(); }
    bool empty() const noexcept { return num_vertices_ == 0; }
    bool is_sorted_neighbors() const noexcept { return sorted_neighbors_; }

    std::size_t edge_begin(VertexId u) const {
        validate_vertex_id(u, num_vertices_);
        return offsets_[u];
    }

    std::size_t edge_end(VertexId u) const {
        validate_vertex_id(u, num_vertices_);
        return offsets_[static_cast<std::size_t>(u) + 1];
    }

    std::size_t out_degree(VertexId u) const {
        validate_vertex_id(u, num_vertices_);
        const std::size_t idx = static_cast<std::size_t>(u);
        return offsets_[idx + 1] - offsets_[idx];
    }

    VertexSpan neighbors(VertexId u) const {
        validate_vertex_id(u, num_vertices_);
        const std::size_t idx = static_cast<std::size_t>(u);
        const std::size_t begin = offsets_[idx];
        const std::size_t end = offsets_[idx + 1];
        if (begin == end) {
            return VertexSpan();
        }
        return VertexSpan(targets_.data() + begin, end - begin);
    }

    bool has_edge(VertexId u, VertexId v) const noexcept {
        if (static_cast<std::size_t>(u) >= num_vertices_ ||
            static_cast<std::size_t>(v) >= num_vertices_) {
            return false;
        }
        const std::size_t idx = static_cast<std::size_t>(u);
        const std::size_t begin = offsets_[idx];
        const std::size_t end = offsets_[idx + 1];
        if (begin == end) {
            return false;
        }
        const VertexId* first = targets_.data() + begin;
        const VertexId* last = targets_.data() + end;
        if (sorted_neighbors_) {
            return std::binary_search(first, last, v);
        }
        return std::find(first, last, v) != last;
    }

    const std::vector<std::size_t>& offsets() const noexcept { return offsets_; }
    const std::vector<VertexId>& targets() const noexcept { return targets_; }

    void clear() noexcept {
        offsets_.clear();
        targets_.clear();
        num_vertices_ = 0;
        sorted_neighbors_ = true;
    }

    template <typename Fn>
    std::size_t multi_source_bfs(const std::vector<VertexId>& sources,
                                 Fn&& fn,
                                 TraversalWorkspace& ws,
                                 const TraversalOptions& opts) const {
        for (std::size_t i = 0; i < sources.size(); ++i) {
            validate_vertex_id(sources[i], num_vertices_);
        }
        if (sources.empty() || opts.limit == 0) {
            return 0;
        }

        ws.prepare(num_vertices_);
        for (std::size_t i = 0; i < sources.size(); ++i) {
            const VertexId s = sources[i];
            if (!ws.is_visited(s)) {
                ws.mark_visited(s);
                ws.bfs_queue_.push_back(TraversalWorkspace::QueueItem{s, 0});
                if (ws.bfs_queue_.size() >= opts.limit) {
                    break;
                }
            }
        }

        std::size_t visited_count = 0;
        std::size_t head = 0;
        while (head < ws.bfs_queue_.size() && visited_count < opts.limit) {
            const TraversalWorkspace::QueueItem cur = ws.bfs_queue_[head++];
            const VisitAction action =
                invoke_traversal_visitor(fn, cur.vertex, cur.depth);
            ++visited_count;
            if (action == VisitAction::Stop || visited_count >= opts.limit) {
                break;
            }
            if (action == VisitAction::SkipBranch || cur.depth >= opts.max_depth) {
                continue;
            }

            const std::size_t next_depth = cur.depth + 1;
            const std::size_t idx = static_cast<std::size_t>(cur.vertex);
            const std::size_t begin = offsets_[idx];
            const std::size_t end = offsets_[idx + 1];
            for (std::size_t e = begin; e < end; ++e) {
                if (ws.bfs_queue_.size() >= opts.limit) {
                    break;
                }
                const VertexId nb = targets_[e];
                if (!ws.is_visited(nb)) {
                    ws.mark_visited(nb);
                    ws.bfs_queue_.push_back(
                        TraversalWorkspace::QueueItem{nb, next_depth});
                }
            }
        }
        return visited_count;
    }

    template <typename Fn>
    std::size_t bfs(VertexId source,
                    Fn&& fn,
                    TraversalWorkspace& ws,
                    const TraversalOptions& opts) const {
        validate_vertex_id(source, num_vertices_);
        if (opts.limit == 0) {
            return 0;
        }

        ws.prepare(num_vertices_);
        ws.mark_visited(source);
        ws.bfs_queue_.push_back(TraversalWorkspace::QueueItem{source, 0});

        std::size_t visited_count = 0;
        std::size_t head = 0;
        while (head < ws.bfs_queue_.size() && visited_count < opts.limit) {
            const TraversalWorkspace::QueueItem cur = ws.bfs_queue_[head++];
            const VisitAction action =
                invoke_traversal_visitor(fn, cur.vertex, cur.depth);
            ++visited_count;
            if (action == VisitAction::Stop || visited_count >= opts.limit) {
                break;
            }
            if (action == VisitAction::SkipBranch || cur.depth >= opts.max_depth) {
                continue;
            }

            const std::size_t next_depth = cur.depth + 1;
            const std::size_t idx = static_cast<std::size_t>(cur.vertex);
            const std::size_t begin = offsets_[idx];
            const std::size_t end = offsets_[idx + 1];
            for (std::size_t e = begin; e < end; ++e) {
                if (ws.bfs_queue_.size() >= opts.limit) {
                    break;
                }
                const VertexId nb = targets_[e];
                if (!ws.is_visited(nb)) {
                    ws.mark_visited(nb);
                    ws.bfs_queue_.push_back(
                        TraversalWorkspace::QueueItem{nb, next_depth});
                }
            }
        }
        return visited_count;
    }

    // Iterative pre-order DFS matching recursive forward neighbor order.
    // `opts.max_depth` limits first-discovery DFS-tree depth (not shortest-hop
    // distance; use bfs() to enumerate all vertices within a hop radius).
    template <typename Fn>
    std::size_t dfs(VertexId source,
                    Fn&& fn,
                    TraversalWorkspace& ws,
                    const TraversalOptions& opts) const {
        validate_vertex_id(source, num_vertices_);
        if (opts.limit == 0) {
            return 0;
        }

        ws.prepare(num_vertices_);
        ws.mark_visited(source);

        std::size_t visited_count = 0;
        const VisitAction root_action = invoke_traversal_visitor(fn, source, 0);
        ++visited_count;
        if (root_action != VisitAction::Continue ||
            visited_count >= opts.limit ||
            opts.max_depth == 0) {
            return visited_count;
        }

        ws.dfs_stack_.push_back(
            TraversalWorkspace::DfsFrame{source, offsets_[source], 0});

        while (!ws.dfs_stack_.empty() && visited_count < opts.limit) {
            TraversalWorkspace::DfsFrame& top = ws.dfs_stack_.back();
            const std::size_t end = offsets_[static_cast<std::size_t>(top.vertex) + 1];
            if (top.edge_idx >= end) {
                ws.dfs_stack_.pop_back();
                continue;
            }

            const VertexId nb = targets_[top.edge_idx++];
            if (ws.is_visited(nb)) {
                continue;
            }

            const std::size_t next_depth = top.depth + 1;
            ws.mark_visited(nb);
            const VisitAction action = invoke_traversal_visitor(fn, nb, next_depth);
            ++visited_count;
            if (action == VisitAction::Stop || visited_count >= opts.limit) {
                break;
            }
            if (action == VisitAction::Continue && next_depth < opts.max_depth) {
                const std::size_t nb_begin = offsets_[nb];
                const std::size_t nb_end = offsets_[static_cast<std::size_t>(nb) + 1];
                if (nb_begin < nb_end) {
                    ws.dfs_stack_.push_back(
                        TraversalWorkspace::DfsFrame{nb, nb_begin, next_depth});
                }
            }
        }
        return visited_count;
    }

    // Unweighted shortest-hop path from `source` to `target` via iterative BFS.
    bool shortest_hops(VertexId source,
                       VertexId target,
                       std::vector<VertexId>& path_out,
                       std::size_t max_depth) const {
        validate_vertex_id(source, num_vertices_);
        validate_vertex_id(target, num_vertices_);

        if (source == target) {
            std::vector<VertexId> result(1, source);
            path_out.swap(result);
            return true;
        }
        if (max_depth == 0) {
            path_out.clear();
            return false;
        }

        std::vector<VertexId> parent(num_vertices_, kInvalidVertex);
        std::vector<std::uint8_t> visited(num_vertices_, 0);
        std::vector<TraversalWorkspace::QueueItem> queue;
        queue.push_back(TraversalWorkspace::QueueItem{source, 0});
        visited[source] = 1;

        bool found = false;
        std::size_t head = 0;
        while (head < queue.size() && !found) {
            const TraversalWorkspace::QueueItem cur = queue[head++];
            if (cur.depth >= max_depth) {
                continue;
            }
            const std::size_t next_depth = cur.depth + 1;
            const std::size_t idx = static_cast<std::size_t>(cur.vertex);
            const std::size_t begin = offsets_[idx];
            const std::size_t end = offsets_[idx + 1];
            for (std::size_t e = begin; e < end; ++e) {
                const VertexId nb = targets_[e];
                if (!visited[nb]) {
                    visited[nb] = 1;
                    parent[nb] = cur.vertex;
                    if (nb == target) {
                        found = true;
                        break;
                    }
                    if (next_depth < max_depth) {
                        queue.push_back(
                            TraversalWorkspace::QueueItem{nb, next_depth});
                    }
                }
            }
        }

        if (!found) {
            path_out.clear();
            return false;
        }

        std::vector<VertexId> rev_path;
        VertexId cur = target;
        while (cur != kInvalidVertex) {
            rev_path.push_back(cur);
            if (cur == source) {
                break;
            }
            cur = parent[cur];
        }
        std::reverse(rev_path.begin(), rev_path.end());
        path_out.swap(rev_path);
        return true;
    }

    bool is_reachable(VertexId source,
                      VertexId target,
                      std::size_t max_depth) const {
        validate_vertex_id(source, num_vertices_);
        validate_vertex_id(target, num_vertices_);
        if (source == target) {
            return true;
        }
        if (max_depth == 0) {
            return false;
        }
        TraversalWorkspace ws;
        TraversalOptions opts;
        opts.max_depth = max_depth;
        bool reached = false;
        bfs(source,
            [&](VertexId v, std::size_t) -> VisitAction {
                if (v == target) {
                    reached = true;
                    return VisitAction::Stop;
                }
                return VisitAction::Continue;
            },
            ws,
            opts);
        return reached;
    }

    bool is_reachable_from_any(const std::vector<VertexId>& sources,
                               VertexId target) const {
        validate_vertex_id(target, num_vertices_);
        if (sources.empty()) {
            return false;
        }
        TraversalWorkspace ws;
        TraversalOptions opts;
        bool reached = false;
        multi_source_bfs(
            sources,
            [&](VertexId v, std::size_t) -> VisitAction {
                if (v == target) {
                    reached = true;
                    return VisitAction::Stop;
                }
                return VisitAction::Continue;
            },
            ws,
            opts);
        return reached;
    }

    // Weakly connected components (treats every edge u -> v as undirected).
    std::vector<std::vector<VertexId> > connected_components() const {
        DisjointSet dsu(num_vertices_);
        for (std::size_t u = 0; u < num_vertices_; ++u) {
            const VertexId uid = static_cast<VertexId>(u);
            const std::size_t begin = offsets_[u];
            const std::size_t end = offsets_[u + 1];
            for (std::size_t e = begin; e < end; ++e) {
                dsu.unite(uid, targets_[e]);
            }
        }
        return dsu.components();
    }

    // Strongly connected components via iterative Tarjan's algorithm.
    // Complexity: O(V + E + sum(|C| log |C|)) because members of each SCC are
    // sorted ascending for deterministic output. SCCs are emitted in reverse
    // topological order of the condensation DAG.
    std::vector<std::vector<VertexId> > strongly_connected_components() const {
        if (num_vertices_ == 0) {
            return std::vector<std::vector<VertexId> >();
        }
        static const std::size_t kUnvisited = std::numeric_limits<std::size_t>::max();
        std::vector<std::size_t> disc(num_vertices_, kUnvisited);
        std::vector<std::size_t> low(num_vertices_, 0);
        std::vector<std::uint8_t> on_stack(num_vertices_, 0);
        std::vector<VertexId> tarjan_stack;
        tarjan_stack.reserve(num_vertices_);

        struct Frame {
            VertexId u;
            std::size_t edge_idx;
        };
        std::vector<Frame> call_stack;
        std::vector<std::vector<VertexId> > sccs;
        std::size_t timer = 0;

        for (std::size_t start = 0; start < num_vertices_; ++start) {
            if (disc[start] != kUnvisited) {
                continue;
            }
            const VertexId start_id = static_cast<VertexId>(start);
            disc[start_id] = low[start_id] = timer++;
            tarjan_stack.push_back(start_id);
            on_stack[start_id] = 1;
            call_stack.push_back(Frame{start_id, offsets_[start]});

            while (!call_stack.empty()) {
                Frame& top = call_stack.back();
                const VertexId u = top.u;
                const std::size_t end = offsets_[static_cast<std::size_t>(u) + 1];

                if (top.edge_idx < end) {
                    const VertexId v = targets_[top.edge_idx++];
                    if (disc[v] == kUnvisited) {
                        disc[v] = low[v] = timer++;
                        tarjan_stack.push_back(v);
                        on_stack[v] = 1;
                        call_stack.push_back(Frame{v, offsets_[v]});
                    } else if (on_stack[v]) {
                        if (disc[v] < low[u]) {
                            low[u] = disc[v];
                        }
                    }
                } else {
                    if (low[u] == disc[u]) {
                        std::vector<VertexId> comp;
                        for (;;) {
                            const VertexId w = tarjan_stack.back();
                            tarjan_stack.pop_back();
                            on_stack[w] = 0;
                            comp.push_back(w);
                            if (w == u) {
                                break;
                            }
                        }
                        std::sort(comp.begin(), comp.end());
                        sccs.push_back(std::move(comp));
                    }
                    const std::size_t finished_low = low[u];
                    call_stack.pop_back();
                    if (!call_stack.empty()) {
                        const VertexId parent = call_stack.back().u;
                        if (finished_low < low[parent]) {
                            low[parent] = finished_low;
                        }
                    }
                }
            }
        }
        return sccs;
    }

private:
    std::vector<std::size_t> offsets_;
    std::vector<VertexId> targets_;
    std::size_t num_vertices_;
    bool sorted_neighbors_;
};

} // namespace detail

// Compact unweighted Compressed Sparse Row (CSR) read-only graph.
//
// Common ads/search/risk-control uses:
//   - Category / taxonomy expansion and K-hop entity recall
//   - Anti-fraud device / account / IP relation subgraph expansion
//   - Zero-stall hot-swappable graph snapshots via DoubleBuffer<CsrGraph>
//
// Properties:
//   - Strict C++11/C++17, header-only, thread-safe for concurrent reads.
//   - Minimal memory layout: 4 bytes per edge + 1 offset per vertex.
//   - 100% iterative BFS/DFS/Tarjan/path reconstruction (safe on deep chains).
//   - Strong exception safety on copy assignment.
class CsrGraph {
public:
    struct BuildOptions {
        bool sort_neighbors;
        bool deduplicate_edges;
        bool allow_self_loops;

        BuildOptions() noexcept
            : sort_neighbors(true),
              deduplicate_edges(false),
              allow_self_loops(true) {}
    };

    class Builder {
    public:
        explicit Builder(std::size_t num_vertices = 0)
            : num_vertices_(0), edges_() {
            detail::validate_vertex_count(num_vertices);
            num_vertices_ = num_vertices;
        }

        void ensure_num_vertices(std::size_t num_vertices) {
            detail::validate_vertex_count(num_vertices);
            if (num_vertices > num_vertices_) {
                num_vertices_ = num_vertices;
            }
        }

        void reserve_edges(std::size_t count) {
            edges_.reserve(count);
        }

        void add_edge(VertexId u, VertexId v) {
            if (u == kInvalidVertex || v == kInvalidVertex) {
                throw std::out_of_range("CsrGraph::Builder vertex id equals kInvalidVertex");
            }
            const std::size_t needed =
                static_cast<std::size_t>(u > v ? u : v) + 1;
            detail::validate_vertex_count(needed);
            edges_.push_back(RawEdge{u, v});
            if (needed > num_vertices_) {
                num_vertices_ = needed;
            }
        }

        void add_undirected_edge(VertexId u, VertexId v) {
            if (u == kInvalidVertex || v == kInvalidVertex) {
                throw std::out_of_range("CsrGraph::Builder vertex id equals kInvalidVertex");
            }
            const std::size_t needed =
                static_cast<std::size_t>(u > v ? u : v) + 1;
            detail::validate_vertex_count(needed);
            const std::size_t prev_vertices = num_vertices_;
            edges_.push_back(RawEdge{u, v});
            if (u != v) {
                try {
                    edges_.push_back(RawEdge{v, u});
                } catch (...) {
                    edges_.pop_back();
                    num_vertices_ = prev_vertices;
                    throw;
                }
            }
            if (needed > num_vertices_) {
                num_vertices_ = needed;
            }
        }

        void clear() noexcept {
            num_vertices_ = 0;
            edges_.clear();
        }

        std::size_t num_vertices() const noexcept { return num_vertices_; }
        std::size_t pending_edges() const noexcept { return edges_.size(); }

        // Builds an immutable CSR snapshot without mutating this builder.
        CsrGraph build(const BuildOptions& opts = BuildOptions()) const {
            if (num_vertices_ == 0) {
                return CsrGraph();
            }
            detail::validate_vertex_count(num_vertices_);

            std::vector<std::size_t> offsets(num_vertices_ + 1, 0);
            for (std::size_t i = 0; i < edges_.size(); ++i) {
                const RawEdge& e = edges_[i];
                if (!opts.allow_self_loops && e.u == e.v) {
                    continue;
                }
                ++offsets[static_cast<std::size_t>(e.u) + 1];
            }

            for (std::size_t i = 0; i < num_vertices_; ++i) {
                offsets[i + 1] += offsets[i];
            }
            const std::size_t total_edges = offsets[num_vertices_];

            std::vector<VertexId> targets(total_edges);
            std::vector<std::size_t> write_pos(
                offsets.begin(),
                offsets.begin() + static_cast<std::ptrdiff_t>(num_vertices_));
            for (std::size_t i = 0; i < edges_.size(); ++i) {
                const RawEdge& e = edges_[i];
                if (!opts.allow_self_loops && e.u == e.v) {
                    continue;
                }
                targets[write_pos[e.u]++] = e.v;
            }

            const bool effective_sorted = opts.sort_neighbors || opts.deduplicate_edges;
            if (effective_sorted) {
                for (std::size_t u = 0; u < num_vertices_; ++u) {
                    std::sort(targets.begin() + static_cast<std::ptrdiff_t>(offsets[u]),
                              targets.begin() + static_cast<std::ptrdiff_t>(offsets[u + 1]));
                }
            }

            if (opts.deduplicate_edges && total_edges > 0) {
                std::vector<std::size_t> compact_offsets(num_vertices_ + 1, 0);
                std::size_t out_idx = 0;
                for (std::size_t u = 0; u < num_vertices_; ++u) {
                    compact_offsets[u] = out_idx;
                    const std::size_t begin = offsets[u];
                    const std::size_t end = offsets[u + 1];
                    for (std::size_t e = begin; e < end; ++e) {
                        if (e == begin || targets[e] != targets[e - 1]) {
                            targets[out_idx++] = targets[e];
                        }
                    }
                }
                compact_offsets[num_vertices_] = out_idx;
                targets.resize(out_idx);
                offsets.swap(compact_offsets);
            }

            return CsrGraph(detail::CsrCore(
                std::move(offsets), std::move(targets), num_vertices_, effective_sorted));
        }

    private:
        struct RawEdge {
            VertexId u;
            VertexId v;
        };

        std::size_t num_vertices_;
        std::vector<RawEdge> edges_;
    };

    CsrGraph() noexcept : core_() {}
    CsrGraph(const CsrGraph&) = default;

    CsrGraph& operator=(const CsrGraph& other) {
        if (this != &other) {
            CsrGraph tmp(other);
            *this = std::move(tmp);
        }
        return *this;
    }

    CsrGraph(CsrGraph&&) noexcept = default;
    CsrGraph& operator=(CsrGraph&&) noexcept = default;

    std::size_t num_vertices() const noexcept { return core_.num_vertices(); }
    std::size_t num_edges() const noexcept { return core_.num_edges(); }
    bool empty() const noexcept { return core_.empty(); }
    bool is_sorted_neighbors() const noexcept { return core_.is_sorted_neighbors(); }

    std::size_t out_degree(VertexId u) const { return core_.out_degree(u); }
    VertexSpan neighbors(VertexId u) const { return core_.neighbors(u); }
    bool has_edge(VertexId u, VertexId v) const noexcept { return core_.has_edge(u, v); }

    template <typename Fn>
    std::size_t bfs(VertexId source,
                    Fn&& fn,
                    const TraversalOptions& opts = TraversalOptions()) const {
        TraversalWorkspace ws;
        return core_.bfs(source, fn, ws, opts);
    }

    template <typename Fn>
    std::size_t bfs(VertexId source,
                    Fn&& fn,
                    TraversalWorkspace& ws,
                    const TraversalOptions& opts = TraversalOptions()) const {
        return core_.bfs(source, fn, ws, opts);
    }

    template <typename Fn>
    std::size_t multi_source_bfs(const std::vector<VertexId>& sources,
                                 Fn&& fn,
                                 const TraversalOptions& opts = TraversalOptions()) const {
        TraversalWorkspace ws;
        return core_.multi_source_bfs(sources, fn, ws, opts);
    }

    template <typename Fn>
    std::size_t multi_source_bfs(const std::vector<VertexId>& sources,
                                 Fn&& fn,
                                 TraversalWorkspace& ws,
                                 const TraversalOptions& opts = TraversalOptions()) const {
        return core_.multi_source_bfs(sources, fn, ws, opts);
    }

    template <typename Fn>
    std::size_t dfs(VertexId source,
                    Fn&& fn,
                    const TraversalOptions& opts = TraversalOptions()) const {
        TraversalWorkspace ws;
        return core_.dfs(source, fn, ws, opts);
    }

    template <typename Fn>
    std::size_t dfs(VertexId source,
                    Fn&& fn,
                    TraversalWorkspace& ws,
                    const TraversalOptions& opts = TraversalOptions()) const {
        return core_.dfs(source, fn, ws, opts);
    }

    bool shortest_path(
        VertexId source,
        VertexId target,
        std::vector<VertexId>& path_out,
        std::size_t max_depth = std::numeric_limits<std::size_t>::max()) const {
        return core_.shortest_hops(source, target, path_out, max_depth);
    }

    bool is_reachable(
        VertexId source,
        VertexId target,
        std::size_t max_depth = std::numeric_limits<std::size_t>::max()) const {
        return core_.is_reachable(source, target, max_depth);
    }

    std::vector<std::vector<VertexId> > connected_components() const {
        return core_.connected_components();
    }

    std::vector<std::vector<VertexId> > strongly_connected_components() const {
        return core_.strongly_connected_components();
    }

    void clear() noexcept { core_.clear(); }

private:
    explicit CsrGraph(detail::CsrCore&& core) noexcept : core_(std::move(core)) {}

    detail::CsrCore core_;
};

// Merge policy for parallel edges (same u -> v) when building WeightedCsrGraph.
enum class EdgeMergePolicy {
    KeepAll,
    KeepMinWeight,
    KeepMaxWeight,
    KeepLast
};

template <typename Weight = double>
struct DijkstraOptions {
    bool has_max_distance;
    Weight max_distance;
    VertexId stop_at_target;

    DijkstraOptions() noexcept
        : has_max_distance(false),
          max_distance(Weight(0)),
          stop_at_target(kInvalidVertex) {}
};

// Shortest-path result returned by Dijkstra, multi-source Dijkstra, and Bellman-Ford.
//
// Guarantees:
//   - Every vertex with `reached[v] != 0` has a finalized optimal shortest distance
//     in `distances[v]`, valid parent chain in `parents`, and originating source in `origins[v]`.
//   - When `complete == false` (early exit via `DijkstraOptions::stop_at_target`),
//     unsettled tentative vertices are cleared so `distance_to()` never leaks
//     non-optimal tentative distances; `is_reachable(v) == false` then means
//     "not settled before early exit" rather than globally unreachable.
template <typename Weight = double>
struct ShortestPathResult {
    // Source vertex for single-source queries; kInvalidVertex for multi-source queries.
    VertexId source;
    bool complete;
    std::vector<Weight> distances;
    std::vector<VertexId> parents;
    // Closest originating source vertex for each finalized vertex.
    std::vector<VertexId> origins;
    std::vector<std::uint8_t> reached;

    ShortestPathResult() noexcept
        : source(kInvalidVertex),
          complete(true),
          distances(),
          parents(),
          origins(),
          reached() {}

    void clear() noexcept {
        source = kInvalidVertex;
        complete = true;
        distances.clear();
        parents.clear();
        origins.clear();
        reached.clear();
    }

    bool is_reachable(VertexId v) const noexcept {
        return static_cast<std::size_t>(v) < reached.size() && reached[v] != 0;
    }

    Weight distance_to(VertexId v) const {
        if (static_cast<std::size_t>(v) >= reached.size()) {
            throw std::out_of_range("ShortestPathResult vertex id out of range");
        }
        if (reached[v] == 0) {
            throw std::out_of_range("ShortestPathResult vertex is unreachable or unsettled");
        }
        return distances[v];
    }

    VertexId nearest_source(VertexId v) const {
        if (static_cast<std::size_t>(v) >= reached.size()) {
            throw std::out_of_range("ShortestPathResult vertex id out of range");
        }
        if (reached[v] == 0) {
            throw std::out_of_range("ShortestPathResult vertex is unreachable or unsettled");
        }
        return origins[v];
    }

    // Reconstructs shortest path [origin_source, ..., target]. Returns false if unreachable/unsettled.
    bool reconstruct_path(VertexId target, std::vector<VertexId>& path_out) const {
        if (!is_reachable(target)) {
            path_out.clear();
            return false;
        }
        std::vector<VertexId> rev_path;
        VertexId cur = target;
        std::size_t steps = 0;
        const std::size_t max_steps = parents.size();
        while (cur != kInvalidVertex) {
            rev_path.push_back(cur);
            if (static_cast<std::size_t>(cur) >= parents.size()) {
                path_out.clear();
                return false;
            }
            const VertexId p = parents[cur];
            if (p == kInvalidVertex) {
                std::reverse(rev_path.begin(), rev_path.end());
                path_out.swap(rev_path);
                return true;
            }
            if (++steps > max_steps) {
                path_out.clear();
                return false;
            }
            cur = p;
        }
        path_out.clear();
        return false;
    }
};

template <typename Weight = double>
using DijkstraResult = ShortestPathResult<Weight>;

template <typename Weight = double>
struct SpanningEdge {
    VertexId u;
    VertexId v;
    Weight weight;
};

template <typename Weight = double>
struct SpanningForestResult {
    std::vector<SpanningEdge<Weight> > edges;
    Weight total_weight;
    std::size_t num_components;

    SpanningForestResult() noexcept
        : edges(), total_weight(Weight(0)), num_components(0) {}
};

// Compact Structure-of-Arrays (SoA) weighted Compressed Sparse Row (CSR) graph.
//
// Storing targets and weights in parallel arrays keeps unweighted BFS/DFS scans
// cache-hot (touching only offsets + targets) while supporting Dijkstra
// (O((V + E) log E) lazy binary heap), multi-source Dijkstra, A*, Bellman-Ford,
// and Kruskal minimum spanning forest.
//
// Immutable after build(): thread-safe for concurrent reads and ideal for
// DoubleBuffer<WeightedCsrGraph<Weight>> lock-free hot reload.
// Strong exception safety on copy assignment.
template <typename Weight = double>
class WeightedCsrGraph {
    static_assert(std::is_arithmetic<Weight>::value &&
                      !std::is_same<Weight, bool>::value,
                  "WeightedCsrGraph Weight must be a non-bool arithmetic type");

public:
    typedef Weight weight_type;
    typedef ConstSpan<Weight> WeightSpan;

    struct BuildOptions {
        bool sort_neighbors;
        EdgeMergePolicy merge_policy;
        bool allow_self_loops;

        BuildOptions() noexcept
            : sort_neighbors(true),
              merge_policy(EdgeMergePolicy::KeepAll),
              allow_self_loops(true) {}
    };

    class Builder {
    public:
        explicit Builder(std::size_t num_vertices = 0)
            : num_vertices_(0), edges_() {
            detail::validate_vertex_count(num_vertices);
            num_vertices_ = num_vertices;
        }

        void ensure_num_vertices(std::size_t num_vertices) {
            detail::validate_vertex_count(num_vertices);
            if (num_vertices > num_vertices_) {
                num_vertices_ = num_vertices;
            }
        }

        void reserve_edges(std::size_t count) {
            edges_.reserve(count);
        }

        void add_edge(VertexId u, VertexId v, Weight weight) {
            if (u == kInvalidVertex || v == kInvalidVertex) {
                throw std::out_of_range(
                    "WeightedCsrGraph::Builder vertex id equals kInvalidVertex");
            }
            if (!detail::is_finite_weight(weight)) {
                throw std::invalid_argument(
                    "WeightedCsrGraph::Builder edge weight must be finite");
            }
            const std::size_t needed =
                static_cast<std::size_t>(u > v ? u : v) + 1;
            detail::validate_vertex_count(needed);
            edges_.push_back(RawEdge{u, v, weight});
            if (needed > num_vertices_) {
                num_vertices_ = needed;
            }
        }

        void add_undirected_edge(VertexId u, VertexId v, Weight weight) {
            if (u == kInvalidVertex || v == kInvalidVertex) {
                throw std::out_of_range(
                    "WeightedCsrGraph::Builder vertex id equals kInvalidVertex");
            }
            if (!detail::is_finite_weight(weight)) {
                throw std::invalid_argument(
                    "WeightedCsrGraph::Builder edge weight must be finite");
            }
            const std::size_t needed =
                static_cast<std::size_t>(u > v ? u : v) + 1;
            detail::validate_vertex_count(needed);
            const std::size_t prev_vertices = num_vertices_;
            edges_.push_back(RawEdge{u, v, weight});
            if (u != v) {
                try {
                    edges_.push_back(RawEdge{v, u, weight});
                } catch (...) {
                    edges_.pop_back();
                    num_vertices_ = prev_vertices;
                    throw;
                }
            }
            if (needed > num_vertices_) {
                num_vertices_ = needed;
            }
        }

        void clear() noexcept {
            num_vertices_ = 0;
            edges_.clear();
        }

        std::size_t num_vertices() const noexcept { return num_vertices_; }
        std::size_t pending_edges() const noexcept { return edges_.size(); }

        // Builds an immutable CSR snapshot without mutating this builder.
        WeightedCsrGraph build(const BuildOptions& opts = BuildOptions()) const {
            if (num_vertices_ == 0) {
                return WeightedCsrGraph();
            }
            detail::validate_vertex_count(num_vertices_);

            std::vector<std::size_t> offsets(num_vertices_ + 1, 0);
            for (std::size_t i = 0; i < edges_.size(); ++i) {
                const RawEdge& e = edges_[i];
                if (!opts.allow_self_loops && e.u == e.v) {
                    continue;
                }
                ++offsets[static_cast<std::size_t>(e.u) + 1];
            }
            for (std::size_t i = 0; i < num_vertices_; ++i) {
                offsets[i + 1] += offsets[i];
            }
            const std::size_t total_edges = offsets[num_vertices_];

            const bool need_sort =
                opts.sort_neighbors || opts.merge_policy != EdgeMergePolicy::KeepAll;

            std::vector<VertexId> targets;
            std::vector<Weight> weights;
            bool has_negative = false;

            if (!need_sort) {
                targets.resize(total_edges);
                weights.resize(total_edges);
                std::vector<std::size_t> write_pos(
                    offsets.begin(),
                    offsets.begin() + static_cast<std::ptrdiff_t>(num_vertices_));
                for (std::size_t i = 0; i < edges_.size(); ++i) {
                    const RawEdge& e = edges_[i];
                    if (!opts.allow_self_loops && e.u == e.v) {
                        continue;
                    }
                    const std::size_t dst = write_pos[e.u]++;
                    targets[dst] = e.v;
                    weights[dst] = e.weight;
                    if (detail::is_negative_weight(e.weight)) {
                        has_negative = true;
                    }
                }
                return WeightedCsrGraph(
                    detail::CsrCore(std::move(offsets), std::move(targets),
                                    num_vertices_, false),
                    std::move(weights),
                    has_negative);
            }

            struct LocalEdge {
                VertexId to;
                Weight weight;
                std::size_t seq;
            };
            std::vector<LocalEdge> bucketed(total_edges);
            std::vector<std::size_t> write_pos(
                offsets.begin(),
                offsets.begin() + static_cast<std::ptrdiff_t>(num_vertices_));
            for (std::size_t i = 0; i < edges_.size(); ++i) {
                const RawEdge& e = edges_[i];
                if (!opts.allow_self_loops && e.u == e.v) {
                    continue;
                }
                bucketed[write_pos[e.u]++] = LocalEdge{e.v, e.weight, i};
            }

            targets.reserve(total_edges);
            weights.reserve(total_edges);
            std::vector<std::size_t> final_offsets(num_vertices_ + 1, 0);

            for (std::size_t u = 0; u < num_vertices_; ++u) {
                final_offsets[u] = targets.size();
                const std::size_t begin = offsets[u];
                const std::size_t end = offsets[u + 1];
                if (begin == end) {
                    continue;
                }

                std::sort(
                    bucketed.begin() + static_cast<std::ptrdiff_t>(begin),
                    bucketed.begin() + static_cast<std::ptrdiff_t>(end),
                    [](const LocalEdge& a, const LocalEdge& b) {
                        if (a.to != b.to) {
                            return a.to < b.to;
                        }
                        return a.seq < b.seq;
                    });

                if (opts.merge_policy == EdgeMergePolicy::KeepAll) {
                    for (std::size_t e = begin; e < end; ++e) {
                        targets.push_back(bucketed[e].to);
                        weights.push_back(bucketed[e].weight);
                        if (detail::is_negative_weight(bucketed[e].weight)) {
                            has_negative = true;
                        }
                    }
                } else {
                    std::size_t e = begin;
                    while (e < end) {
                        const VertexId to = bucketed[e].to;
                        Weight chosen = bucketed[e].weight;
                        ++e;
                        while (e < end && bucketed[e].to == to) {
                            const Weight cand = bucketed[e].weight;
                            if (opts.merge_policy == EdgeMergePolicy::KeepMinWeight) {
                                if (cand < chosen) {
                                    chosen = cand;
                                }
                            } else if (opts.merge_policy == EdgeMergePolicy::KeepMaxWeight) {
                                if (cand > chosen) {
                                    chosen = cand;
                                }
                            } else {
                                chosen = cand;
                            }
                            ++e;
                        }
                        targets.push_back(to);
                        weights.push_back(chosen);
                        if (detail::is_negative_weight(chosen)) {
                            has_negative = true;
                        }
                    }
                }
            }
            final_offsets[num_vertices_] = targets.size();

            return WeightedCsrGraph(
                detail::CsrCore(std::move(final_offsets), std::move(targets),
                                num_vertices_, true),
                std::move(weights),
                has_negative);
        }

    private:
        struct RawEdge {
            VertexId u;
            VertexId v;
            Weight weight;
        };

        std::size_t num_vertices_;
        std::vector<RawEdge> edges_;
    };

    WeightedCsrGraph() noexcept
        : core_(), weights_(), has_negative_weight_(false) {}
    WeightedCsrGraph(const WeightedCsrGraph&) = default;

    WeightedCsrGraph& operator=(const WeightedCsrGraph& other) {
        if (this != &other) {
            WeightedCsrGraph tmp(other);
            *this = std::move(tmp);
        }
        return *this;
    }

    WeightedCsrGraph(WeightedCsrGraph&& other) noexcept
        : core_(std::move(other.core_)),
          weights_(std::move(other.weights_)),
          has_negative_weight_(other.has_negative_weight_) {
        other.weights_.clear();
        other.has_negative_weight_ = false;
    }

    WeightedCsrGraph& operator=(WeightedCsrGraph&& other) noexcept {
        if (this != &other) {
            core_ = std::move(other.core_);
            weights_ = std::move(other.weights_);
            has_negative_weight_ = other.has_negative_weight_;
            other.weights_.clear();
            other.has_negative_weight_ = false;
        }
        return *this;
    }

    std::size_t num_vertices() const noexcept { return core_.num_vertices(); }
    std::size_t num_edges() const noexcept { return core_.num_edges(); }
    bool empty() const noexcept { return core_.empty(); }
    bool is_sorted_neighbors() const noexcept { return core_.is_sorted_neighbors(); }
    bool has_negative_weights() const noexcept { return has_negative_weight_; }

    std::size_t out_degree(VertexId u) const { return core_.out_degree(u); }
    VertexSpan neighbors(VertexId u) const { return core_.neighbors(u); }

    WeightSpan weights(VertexId u) const {
        const std::size_t begin = core_.edge_begin(u);
        const std::size_t end = core_.edge_end(u);
        if (begin == end) {
            return WeightSpan();
        }
        return WeightSpan(weights_.data() + begin, end - begin);
    }

    bool has_edge(VertexId u, VertexId v) const noexcept { return core_.has_edge(u, v); }

    template <typename Fn>
    void for_each_edge(VertexId u, Fn&& fn) const {
        const std::size_t begin = core_.edge_begin(u);
        const std::size_t end = core_.edge_end(u);
        const VertexId* tgt = core_.targets().data();
        const Weight* wts = weights_.data();
        for (std::size_t e = begin; e < end; ++e) {
            fn(tgt[e], wts[e]);
        }
    }

    template <typename Fn>
    std::size_t bfs(VertexId source,
                    Fn&& fn,
                    const TraversalOptions& opts = TraversalOptions()) const {
        TraversalWorkspace ws;
        return core_.bfs(source, fn, ws, opts);
    }

    template <typename Fn>
    std::size_t bfs(VertexId source,
                    Fn&& fn,
                    TraversalWorkspace& ws,
                    const TraversalOptions& opts = TraversalOptions()) const {
        return core_.bfs(source, fn, ws, opts);
    }

    template <typename Fn>
    std::size_t multi_source_bfs(const std::vector<VertexId>& sources,
                                 Fn&& fn,
                                 const TraversalOptions& opts = TraversalOptions()) const {
        TraversalWorkspace ws;
        return core_.multi_source_bfs(sources, fn, ws, opts);
    }

    template <typename Fn>
    std::size_t multi_source_bfs(const std::vector<VertexId>& sources,
                                 Fn&& fn,
                                 TraversalWorkspace& ws,
                                 const TraversalOptions& opts = TraversalOptions()) const {
        return core_.multi_source_bfs(sources, fn, ws, opts);
    }

    template <typename Fn>
    std::size_t dfs(VertexId source,
                    Fn&& fn,
                    const TraversalOptions& opts = TraversalOptions()) const {
        TraversalWorkspace ws;
        return core_.dfs(source, fn, ws, opts);
    }

    template <typename Fn>
    std::size_t dfs(VertexId source,
                    Fn&& fn,
                    TraversalWorkspace& ws,
                    const TraversalOptions& opts = TraversalOptions()) const {
        return core_.dfs(source, fn, ws, opts);
    }

    bool shortest_hops(
        VertexId source,
        VertexId target,
        std::vector<VertexId>& path_out,
        std::size_t max_depth = std::numeric_limits<std::size_t>::max()) const {
        return core_.shortest_hops(source, target, path_out, max_depth);
    }

    bool is_reachable(
        VertexId source,
        VertexId target,
        std::size_t max_depth = std::numeric_limits<std::size_t>::max()) const {
        return core_.is_reachable(source, target, max_depth);
    }

    std::vector<std::vector<VertexId> > connected_components() const {
        return core_.connected_components();
    }

    std::vector<std::vector<VertexId> > strongly_connected_components() const {
        return core_.strongly_connected_components();
    }

    // Single-source Dijkstra shortest paths for non-negative edge weights.
    // Complexity: O((V + E) log E) with a lazy binary min-heap.
    // Throws:
    //   - std::out_of_range if source or stop_at_target is invalid.
    //   - std::domain_error if the graph contains any negative edge weight.
    //   - std::invalid_argument if max_distance is negative or non-finite.
    //   - std::overflow_error if the true shortest distance to a reachable
    //     vertex (or stop_at_target) exceeds Weight's representable maximum.
    ShortestPathResult<Weight> dijkstra(
        VertexId source,
        const DijkstraOptions<Weight>& opts = DijkstraOptions<Weight>()) const {
        const VertexId src_arr[1] = {source};
        ShortestPathResult<Weight> res = dijkstra_impl(src_arr, 1, opts);
        res.source = source;
        return res;
    }

    // Multi-source Dijkstra shortest paths (O((V + E) log E) lazy binary heap).
    ShortestPathResult<Weight> multi_source_dijkstra(
        const std::vector<VertexId>& sources,
        const DijkstraOptions<Weight>& opts = DijkstraOptions<Weight>()) const {
        return dijkstra_impl(sources.data(), sources.size(), opts);
    }

    // Point-to-point A* heuristic shortest path for non-negative edge weights.
    // `heuristic(VertexId v)` must return a finite, non-negative arithmetic
    // lower bound on the remaining distance from `v` to `target`.
    // Supports vertex re-opening for admissible non-consistent heuristics.
    template <typename HeuristicFn>
    bool astar(VertexId source,
               VertexId target,
               HeuristicFn&& heuristic,
               std::vector<VertexId>& path_out,
               Weight* total_distance_out = nullptr) const {
        return astar_impl(source, target, false, Weight(0),
                          heuristic, path_out, total_distance_out);
    }

    template <typename HeuristicFn>
    bool astar(VertexId source,
               VertexId target,
               Weight max_distance,
               HeuristicFn&& heuristic,
               std::vector<VertexId>& path_out,
               Weight* total_distance_out = nullptr) const {
        return astar_impl(source, target, true, max_distance,
                          heuristic, path_out, total_distance_out);
    }

    // Single-source Bellman-Ford shortest paths supporting negative edge weights.
    // Returns true and populates `result_out` if no negative-weight cycle is
    // reachable from `source`. Returns false and clears `result_out` if a
    // reachable negative-weight cycle is detected.
    //
    // Precision & overflow semantics:
    //   - Integral Weight (up to 64-bit): runs internally in exact 128-bit
    //     two's-complement arithmetic, so reachable negative cycles always
    //     return false (never spurious overflow_error) and intermediate paths
    //     that exceed Weight's range never mask representable shorter paths.
    //     Throws std::overflow_error only if a final true shortest distance
    //     cannot be represented in Weight.
    //   - Floating-point Weight: runs internally in long double; subject to
    //     IEEE floating-point rounding / loss of significance if edge magnitudes
    //     differ by more than the significand precision.
    bool bellman_ford(VertexId source, ShortestPathResult<Weight>& result_out) const {
        return bellman_ford_impl(source, result_out,
                                 std::is_integral<Weight>());
    }

    // Minimum Spanning Forest via Kruskal's algorithm using DisjointSet (O(E log E)).
    // Treats every edge (u, v, w) as undirected and ignores self-loops.
    // Supports disconnected graphs (returns spanning forest) and negative weights.
    //
    // For integral Weight, accumulates in exact 128-bit arithmetic and checks
    // range only on the final representable forest total (so order-dependent
    // intermediate prefixes like INT_MIN + (-1) + INT_MAX == -2 succeed).
    // Throws std::overflow_error if the final total cannot be represented in Weight.
    SpanningForestResult<Weight> minimum_spanning_forest() const {
        const std::size_t n = core_.num_vertices();
        SpanningForestResult<Weight> result;
        if (n == 0) {
            return result;
        }

        const std::vector<std::size_t>& offsets = core_.offsets();
        const std::vector<VertexId>& targets = core_.targets();

        std::vector<SpanningEdge<Weight> > all_edges;
        all_edges.reserve(targets.size());
        for (std::size_t u = 0; u < n; ++u) {
            const VertexId uid = static_cast<VertexId>(u);
            const std::size_t begin = offsets[u];
            const std::size_t end = offsets[u + 1];
            for (std::size_t e = begin; e < end; ++e) {
                const VertexId v = targets[e];
                if (uid == v) {
                    continue;
                }
                const VertexId a = (uid < v) ? uid : v;
                const VertexId b = (uid < v) ? v : uid;
                all_edges.push_back(SpanningEdge<Weight>{a, b, weights_[e]});
            }
        }

        std::sort(all_edges.begin(), all_edges.end(),
                  [](const SpanningEdge<Weight>& lhs, const SpanningEdge<Weight>& rhs) {
                      if (lhs.weight != rhs.weight) {
                          return lhs.weight < rhs.weight;
                      }
                      if (lhs.u != rhs.u) {
                          return lhs.u < rhs.u;
                      }
                      return lhs.v < rhs.v;
                  });

        DisjointSet dsu(n);
        const std::size_t reserve_cap =
            ((n - 1) < all_edges.size()) ? (n - 1) : all_edges.size();
        result.edges.reserve(reserve_cap);

        for (std::size_t i = 0; i < all_edges.size() && dsu.num_components() > 1; ++i) {
            const SpanningEdge<Weight>& edge = all_edges[i];
            if (dsu.unite(edge.u, edge.v)) {
                result.edges.push_back(edge);
            }
        }
        result.total_weight = sum_spanning_weights(result.edges, std::is_integral<Weight>());
        result.num_components = dsu.num_components();
        return result;
    }

    // Convenience point-to-point weighted shortest path using early-exit Dijkstra.
    bool shortest_path(VertexId source,
                       VertexId target,
                       std::vector<VertexId>& path_out,
                       Weight* total_distance_out = nullptr) const {
        detail::validate_vertex_id(source, core_.num_vertices());
        detail::validate_vertex_id(target, core_.num_vertices());
        DijkstraOptions<Weight> opts;
        opts.stop_at_target = target;
        const ShortestPathResult<Weight> res = dijkstra(source, opts);
        if (!res.reconstruct_path(target, path_out)) {
            return false;
        }
        if (total_distance_out != nullptr) {
            *total_distance_out = res.distances[target];
        }
        return true;
    }

    bool shortest_path(VertexId source,
                       VertexId target,
                       Weight max_distance,
                       std::vector<VertexId>& path_out,
                       Weight* total_distance_out = nullptr) const {
        detail::validate_vertex_id(source, core_.num_vertices());
        detail::validate_vertex_id(target, core_.num_vertices());
        DijkstraOptions<Weight> opts;
        opts.has_max_distance = true;
        opts.max_distance = max_distance;
        opts.stop_at_target = target;
        const ShortestPathResult<Weight> res = dijkstra(source, opts);
        if (!res.reconstruct_path(target, path_out)) {
            return false;
        }
        if (total_distance_out != nullptr) {
            *total_distance_out = res.distances[target];
        }
        return true;
    }

    void clear() noexcept {
        core_.clear();
        weights_.clear();
        has_negative_weight_ = false;
    }

private:
    static Weight sum_spanning_weights(const std::vector<SpanningEdge<Weight> >& edges,
                                       std::true_type /*integral*/) {
        detail::Int128 acc;
        for (std::size_t i = 0; i < edges.size(); ++i) {
            acc = acc + detail::Int128::from(edges[i].weight);
        }
        Weight out = Weight(0);
        if (!acc.try_to(out)) {
            throw std::overflow_error("minimum_spanning_forest total weight overflow");
        }
        return out;
    }

    static Weight sum_spanning_weights(const std::vector<SpanningEdge<Weight> >& edges,
                                       std::false_type /*floating_point*/) {
        // Neumaier compensated summation in long double minimizes cancellation error.
        long double sum = 0.0L;
        long double comp = 0.0L;
        for (std::size_t i = 0; i < edges.size(); ++i) {
            const long double x = static_cast<long double>(edges[i].weight);
            const long double t = sum + x;
            if (!std::isfinite(t)) {
                throw std::overflow_error("minimum_spanning_forest total weight overflow");
            }
            if (std::fabs(sum) >= std::fabs(x)) {
                comp += (sum - t) + x;
            } else {
                comp += (x - t) + sum;
            }
            sum = t;
        }
        const long double total_ld = sum + comp;
        const Weight out = static_cast<Weight>(total_ld);
        if (!std::isfinite(total_ld) || !std::isfinite(out)) {
            throw std::overflow_error("minimum_spanning_forest total weight overflow");
        }
        return out;
    }

    bool bellman_ford_impl(VertexId source,
                           ShortestPathResult<Weight>& result_out,
                           std::true_type /*integral*/) const {
        const std::size_t n = core_.num_vertices();
        detail::validate_vertex_id(source, n);

        std::vector<detail::Int128> dist128(n);
        std::vector<VertexId> parents(n, kInvalidVertex);
        std::vector<VertexId> origins(n, kInvalidVertex);
        std::vector<std::uint8_t> reached(n, 0);

        reached[source] = 1;
        origins[source] = source;

        const std::vector<std::size_t>& offsets = core_.offsets();
        const std::vector<VertexId>& targets = core_.targets();

        bool updated = false;
        for (std::size_t pass = 0; pass + 1 < n; ++pass) {
            updated = false;
            for (std::size_t u = 0; u < n; ++u) {
                if (!reached[u]) {
                    continue;
                }
                const detail::Int128 du = dist128[u];
                const std::size_t begin = offsets[u];
                const std::size_t end = offsets[u + 1];
                for (std::size_t e = begin; e < end; ++e) {
                    const VertexId v = targets[e];
                    const detail::Int128 cand = du + detail::Int128::from(weights_[e]);
                    if (!reached[v] || cand < dist128[v]) {
                        reached[v] = 1;
                        dist128[v] = cand;
                        parents[v] = static_cast<VertexId>(u);
                        origins[v] = source;
                        updated = true;
                    }
                }
            }
            if (!updated) {
                break;
            }
        }

        // N-th pass detects any reachable negative-weight cycle (including n == 1).
        if (updated || n == 1) {
            for (std::size_t u = 0; u < n; ++u) {
                if (!reached[u]) {
                    continue;
                }
                const detail::Int128 du = dist128[u];
                const std::size_t begin = offsets[u];
                const std::size_t end = offsets[u + 1];
                for (std::size_t e = begin; e < end; ++e) {
                    const VertexId v = targets[e];
                    const detail::Int128 cand = du + detail::Int128::from(weights_[e]);
                    if (!reached[v] || cand < dist128[v]) {
                        result_out.clear();
                        return false;
                    }
                }
            }
        }

        ShortestPathResult<Weight> res;
        res.source = source;
        res.complete = true;
        res.distances.assign(n, Weight(0));
        for (std::size_t v = 0; v < n; ++v) {
            if (reached[v]) {
                if (!dist128[v].try_to(res.distances[v])) {
                    throw std::overflow_error("Bellman-Ford path distance overflow");
                }
            }
        }
        res.parents.swap(parents);
        res.origins.swap(origins);
        res.reached.swap(reached);
        result_out = std::move(res);
        return true;
    }

    bool bellman_ford_impl(VertexId source,
                           ShortestPathResult<Weight>& result_out,
                           std::false_type /*floating_point*/) const {
        const std::size_t n = core_.num_vertices();
        detail::validate_vertex_id(source, n);

        std::vector<long double> dist_ld(n, 0.0L);
        std::vector<VertexId> parents(n, kInvalidVertex);
        std::vector<VertexId> origins(n, kInvalidVertex);
        std::vector<std::uint8_t> reached(n, 0);
        std::vector<std::uint8_t> pos_overflow(n, 0);

        reached[source] = 1;
        origins[source] = source;

        const std::vector<std::size_t>& offsets = core_.offsets();
        const std::vector<VertexId>& targets = core_.targets();

        bool updated = false;
        for (std::size_t pass = 0; pass + 1 < n; ++pass) {
            updated = false;
            for (std::size_t u = 0; u < n; ++u) {
                if (!reached[u]) {
                    continue;
                }
                const long double du = dist_ld[u];
                const std::size_t begin = offsets[u];
                const std::size_t end = offsets[u + 1];
                for (std::size_t e = begin; e < end; ++e) {
                    const VertexId v = targets[e];
                    const long double cand = du + static_cast<long double>(weights_[e]);
                    if (!std::isfinite(cand)) {
                        if (cand > 0.0L) {
                            if (!reached[v]) {
                                pos_overflow[v] = 1;
                            }
                            continue;
                        }
                        throw std::overflow_error("Bellman-Ford path distance underflow");
                    }
                    if (!reached[v] || cand < dist_ld[v]) {
                        reached[v] = 1;
                        pos_overflow[v] = 0;
                        dist_ld[v] = cand;
                        parents[v] = static_cast<VertexId>(u);
                        origins[v] = source;
                        updated = true;
                    }
                }
            }
            if (!updated) {
                break;
            }
        }

        if (updated || n == 1) {
            for (std::size_t u = 0; u < n; ++u) {
                if (!reached[u]) {
                    continue;
                }
                const long double du = dist_ld[u];
                const std::size_t begin = offsets[u];
                const std::size_t end = offsets[u + 1];
                for (std::size_t e = begin; e < end; ++e) {
                    const VertexId v = targets[e];
                    const long double cand = du + static_cast<long double>(weights_[e]);
                    if (!std::isfinite(cand)) {
                        if (cand > 0.0L) {
                            continue;
                        }
                        result_out.clear();
                        return false;
                    }
                    if (!reached[v] || cand < dist_ld[v]) {
                        result_out.clear();
                        return false;
                    }
                }
            }
        }

        ShortestPathResult<Weight> res;
        res.source = source;
        res.complete = true;
        res.distances.assign(n, Weight(0));
        for (std::size_t v = 0; v < n; ++v) {
            if (pos_overflow[v] && !reached[v]) {
                throw std::overflow_error("Bellman-Ford path distance overflow");
            }
            if (reached[v]) {
                const Weight conv = static_cast<Weight>(dist_ld[v]);
                if (!std::isfinite(conv)) {
                    throw std::overflow_error("Bellman-Ford path distance overflow");
                }
                res.distances[v] = conv;
            }
        }
        res.parents.swap(parents);
        res.origins.swap(origins);
        res.reached.swap(reached);
        result_out = std::move(res);
        return true;
    }

    ShortestPathResult<Weight> dijkstra_impl(
        const VertexId* sources,
        std::size_t num_sources,
        const DijkstraOptions<Weight>& opts) const {
        const std::size_t n = core_.num_vertices();
        for (std::size_t i = 0; i < num_sources; ++i) {
            detail::validate_vertex_id(sources[i], n);
        }
        if (opts.stop_at_target != kInvalidVertex) {
            detail::validate_vertex_id(opts.stop_at_target, n);
        }
        if (has_negative_weight_) {
            throw std::domain_error("Dijkstra requires non-negative edge weights");
        }
        if (opts.has_max_distance) {
            if (!detail::is_finite_weight(opts.max_distance) ||
                detail::is_negative_weight(opts.max_distance)) {
                throw std::invalid_argument(
                    "Dijkstra max_distance must be finite and non-negative");
            }
        }

        ShortestPathResult<Weight> res;
        res.source = kInvalidVertex;
        res.complete = true;
        res.distances.assign(n, Weight(0));
        res.parents.assign(n, kInvalidVertex);
        res.origins.assign(n, kInvalidVertex);
        res.reached.assign(n, 0);

        if (num_sources == 0) {
            return res;
        }

        struct HeapEntry {
            Weight dist;
            VertexId vertex;
        };
        struct HeapGreater {
            bool operator()(const HeapEntry& a, const HeapEntry& b) const noexcept {
                if (a.dist != b.dist) {
                    return a.dist > b.dist;
                }
                return a.vertex > b.vertex;
            }
        };

        std::vector<std::uint8_t> settled(n, 0);
        std::vector<std::uint8_t> overflowed(n, 0);
        bool any_overflow = false;
        std::priority_queue<HeapEntry, std::vector<HeapEntry>, HeapGreater> pq;

        for (std::size_t i = 0; i < num_sources; ++i) {
            const VertexId s = sources[i];
            if (!res.reached[s]) {
                res.reached[s] = 1;
                res.distances[s] = Weight(0);
                res.origins[s] = s;
                pq.push(HeapEntry{Weight(0), s});
            }
        }

        const std::vector<std::size_t>& offsets = core_.offsets();
        const std::vector<VertexId>& targets = core_.targets();
        bool stopped_early = false;

        while (!pq.empty()) {
            const HeapEntry cur = pq.top();
            pq.pop();

            if (settled[cur.vertex]) {
                continue;
            }
            settled[cur.vertex] = 1;
            if (cur.vertex == opts.stop_at_target) {
                stopped_early = true;
                break;
            }

            const std::size_t uidx = static_cast<std::size_t>(cur.vertex);
            const std::size_t begin = offsets[uidx];
            const std::size_t end = offsets[uidx + 1];
            for (std::size_t e = begin; e < end; ++e) {
                const VertexId nb = targets[e];
                if (settled[nb]) {
                    continue;
                }
                Weight cand = Weight(0);
                const detail::AddCutoffStatus st = detail::add_nonneg_with_cutoff(
                    cur.dist, weights_[e], opts.has_max_distance, opts.max_distance, cand);
                if (st == detail::AddCutoffStatus::ExceedsCutoff) {
                    continue;
                }
                if (st == detail::AddCutoffStatus::Overflow) {
                    if (!res.reached[nb]) {
                        overflowed[nb] = 1;
                        any_overflow = true;
                    }
                    continue;
                }
                if (!res.reached[nb] || cand < res.distances[nb]) {
                    res.reached[nb] = 1;
                    overflowed[nb] = 0;
                    res.distances[nb] = cand;
                    res.parents[nb] = cur.vertex;
                    res.origins[nb] = res.origins[cur.vertex];
                    pq.push(HeapEntry{cand, nb});
                }
            }
        }

        if (stopped_early) {
            // Clear unsettled tentative discoveries so result only reports
            // finalized optimal shortest distances.
            bool had_unsettled = false;
            for (std::size_t v = 0; v < n; ++v) {
                if (res.reached[v] && !settled[v]) {
                    res.reached[v] = 0;
                    res.distances[v] = Weight(0);
                    res.parents[v] = kInvalidVertex;
                    res.origins[v] = kInvalidVertex;
                    had_unsettled = true;
                }
            }
            res.complete = !had_unsettled && pq.empty();
            return res;
        }

        // Only raise overflow_error if a vertex (or requested stop_at_target) had
        // NO representable shortest path and was blocked by Weight overflow.
        if (any_overflow) {
            std::vector<VertexId> ov_nodes;
            for (std::size_t v = 0; v < n; ++v) {
                if (overflowed[v] && !res.reached[v]) {
                    ov_nodes.push_back(static_cast<VertexId>(v));
                }
            }
            if (!ov_nodes.empty()) {
                if (opts.stop_at_target == kInvalidVertex) {
                    throw std::overflow_error("Dijkstra path distance overflow");
                }
                if (core_.is_reachable_from_any(ov_nodes, opts.stop_at_target)) {
                    throw std::overflow_error("Dijkstra path distance overflow");
                }
            }
        }

        return res;
    }

    template <typename HeuristicFn>
    bool astar_impl(VertexId source,
                    VertexId target,
                    bool has_max_distance,
                    Weight max_distance,
                    HeuristicFn& heuristic,
                    std::vector<VertexId>& path_out,
                    Weight* total_distance_out) const {
        const std::size_t n = core_.num_vertices();
        detail::validate_vertex_id(source, n);
        detail::validate_vertex_id(target, n);
        if (has_negative_weight_) {
            throw std::domain_error("A* requires non-negative edge weights");
        }
        if (has_max_distance) {
            if (!detail::is_finite_weight(max_distance) ||
                detail::is_negative_weight(max_distance)) {
                throw std::invalid_argument(
                    "A* max_distance must be finite and non-negative");
            }
        }

        if (source == target) {
            std::vector<VertexId> result(1, source);
            path_out.swap(result);
            if (total_distance_out != nullptr) {
                *total_distance_out = Weight(0);
            }
            return true;
        }

        Weight h_src = Weight(0);
        const detail::AddCutoffStatus h_src_st =
            detail::validate_and_convert_heuristic(heuristic(source), h_src);
        if (h_src_st == detail::AddCutoffStatus::Overflow) {
            if (has_max_distance) {
                path_out.clear();
                return false;
            }
            throw std::overflow_error("A* heuristic exceeds Weight capacity");
        }
        if (has_max_distance && h_src > max_distance) {
            path_out.clear();
            return false;
        }

        struct HeapEntry {
            Weight f;
            Weight g;
            VertexId vertex;
        };
        struct HeapGreater {
            bool operator()(const HeapEntry& a, const HeapEntry& b) const noexcept {
                if (a.f != b.f) {
                    return a.f > b.f;
                }
                if (a.g != b.g) {
                    return a.g > b.g;
                }
                return a.vertex > b.vertex;
            }
        };

        std::vector<Weight> g_score(n, Weight(0));
        std::vector<VertexId> parent(n, kInvalidVertex);
        std::vector<std::uint8_t> seen(n, 0);
        std::vector<std::uint8_t> overflowed(n, 0);
        bool any_overflow = false;
        std::priority_queue<HeapEntry, std::vector<HeapEntry>, HeapGreater> pq;

        seen[source] = 1;
        g_score[source] = Weight(0);
        pq.push(HeapEntry{h_src, Weight(0), source});

        const std::vector<std::size_t>& offsets = core_.offsets();
        const std::vector<VertexId>& targets = core_.targets();
        bool found = false;

        while (!pq.empty()) {
            const HeapEntry cur = pq.top();
            pq.pop();

            if (cur.g > g_score[cur.vertex]) {
                continue;
            }
            if (cur.vertex == target) {
                found = true;
                break;
            }

            const std::size_t uidx = static_cast<std::size_t>(cur.vertex);
            const std::size_t begin = offsets[uidx];
            const std::size_t end = offsets[uidx + 1];
            for (std::size_t e = begin; e < end; ++e) {
                const VertexId nb = targets[e];
                Weight next_g = Weight(0);
                const detail::AddCutoffStatus g_st = detail::add_nonneg_with_cutoff(
                    cur.g, weights_[e], has_max_distance, max_distance, next_g);
                if (g_st == detail::AddCutoffStatus::ExceedsCutoff) {
                    continue;
                }
                if (g_st == detail::AddCutoffStatus::Overflow) {
                    if (!seen[nb]) {
                        overflowed[nb] = 1;
                        any_overflow = true;
                    }
                    continue;
                }
                if (seen[nb] && !(next_g < g_score[nb])) {
                    continue;
                }

                Weight h_nb = Weight(0);
                if (nb != target) {
                    const detail::AddCutoffStatus h_st =
                        detail::validate_and_convert_heuristic(heuristic(nb), h_nb);
                    if (h_st == detail::AddCutoffStatus::Overflow) {
                        if (has_max_distance) {
                            continue;
                        }
                        overflowed[nb] = 1;
                        any_overflow = true;
                        continue;
                    }
                }

                Weight next_f = Weight(0);
                const detail::AddCutoffStatus f_st = detail::add_nonneg_with_cutoff(
                    next_g, h_nb, has_max_distance, max_distance, next_f);
                if (f_st == detail::AddCutoffStatus::ExceedsCutoff) {
                    continue;
                }
                if (f_st == detail::AddCutoffStatus::Overflow) {
                    overflowed[nb] = 1;
                    any_overflow = true;
                    continue;
                }

                seen[nb] = 1;
                overflowed[nb] = 0;
                g_score[nb] = next_g;
                parent[nb] = cur.vertex;
                pq.push(HeapEntry{next_f, next_g, nb});
            }
        }

        if (!found) {
            path_out.clear();
            if (any_overflow) {
                std::vector<VertexId> ov_nodes;
                for (std::size_t v = 0; v < n; ++v) {
                    if (overflowed[v]) {
                        ov_nodes.push_back(static_cast<VertexId>(v));
                    }
                }
                if (!ov_nodes.empty() && core_.is_reachable_from_any(ov_nodes, target)) {
                    throw std::overflow_error("A* path distance overflow");
                }
            }
            return false;
        }

        std::vector<VertexId> rev_path;
        VertexId cur = target;
        std::size_t steps = 0;
        while (cur != kInvalidVertex) {
            rev_path.push_back(cur);
            if (cur == source) {
                std::reverse(rev_path.begin(), rev_path.end());
                path_out.swap(rev_path);
                if (total_distance_out != nullptr) {
                    *total_distance_out = g_score[target];
                }
                return true;
            }
            if (++steps > n) {
                path_out.clear();
                return false;
            }
            cur = parent[cur];
        }
        path_out.clear();
        return false;
    }

    WeightedCsrGraph(detail::CsrCore&& core,
                     std::vector<Weight>&& weights,
                     bool has_negative_weight) noexcept
        : core_(std::move(core)),
          weights_(std::move(weights)),
          has_negative_weight_(has_negative_weight) {}

    detail::CsrCore core_;
    std::vector<Weight> weights_;
    bool has_negative_weight_;
};

} // namespace search
} // namespace myframe
