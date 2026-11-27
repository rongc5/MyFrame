#include "search/search.h"
#include "pipeline/pipeline.h"
#include "containers/double_buffer.h"
#include "task_executor.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <queue>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define MYFRAME_TEST_ALLOC_HOOK 0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define MYFRAME_TEST_ALLOC_HOOK 0
#else
#define MYFRAME_TEST_ALLOC_HOOK 1
#endif
#else
#define MYFRAME_TEST_ALLOC_HOOK 1
#endif

namespace {

thread_local bool g_alloc_tracking = false;
thread_local bool g_alloc_fail_armed = false;
thread_local std::size_t g_alloc_fail_after = 0;
thread_local std::size_t g_alloc_calls = 0;

#if MYFRAME_TEST_ALLOC_HOOK
void* test_alloc_checked(std::size_t size, bool allow_throw) {
    if (g_alloc_tracking) {
        ++g_alloc_calls;
        if (g_alloc_fail_armed) {
            if (g_alloc_fail_after == 0) {
                if (allow_throw) {
                    throw std::bad_alloc();
                }
                return nullptr;
            }
            --g_alloc_fail_after;
        }
    }
    void* ptr = std::malloc(size == 0 ? 1 : size);
    if (!ptr && allow_throw) {
        throw std::bad_alloc();
    }
    return ptr;
}
#endif

struct AllocFaultScope {
    bool prev_tracking;
    bool prev_armed;
    std::size_t prev_after;
    std::size_t prev_calls;

    // Pass SIZE_MAX to count allocations without injecting failures.
    explicit AllocFaultScope(std::size_t fail_after) noexcept
        : prev_tracking(g_alloc_tracking),
          prev_armed(g_alloc_fail_armed),
          prev_after(g_alloc_fail_after),
          prev_calls(g_alloc_calls) {
        g_alloc_calls = 0;
        g_alloc_tracking = true;
        if (fail_after == std::numeric_limits<std::size_t>::max()) {
            g_alloc_fail_armed = false;
            g_alloc_fail_after = 0;
        } else {
            g_alloc_fail_armed = true;
            g_alloc_fail_after = fail_after;
        }
    }

    ~AllocFaultScope() noexcept {
        g_alloc_tracking = prev_tracking;
        g_alloc_fail_armed = prev_armed;
        g_alloc_fail_after = prev_after;
        g_alloc_calls = prev_calls;
    }

    std::size_t calls() const noexcept { return g_alloc_calls; }
};

} // namespace

#if MYFRAME_TEST_ALLOC_HOOK
void* operator new(std::size_t size) {
    return test_alloc_checked(size, true);
}

void* operator new[](std::size_t size) {
    return test_alloc_checked(size, true);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return test_alloc_checked(size, false);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return test_alloc_checked(size, false);
}

void operator delete(void* ptr) noexcept {
    std::free(ptr);
}

void operator delete[](void* ptr) noexcept {
    std::free(ptr);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept {
    std::free(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
    std::free(ptr);
}

#if defined(__cpp_sized_deallocation) && __cpp_sized_deallocation >= 201309
void operator delete(void* ptr, std::size_t) noexcept {
    std::free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
    std::free(ptr);
}
#endif
#endif

namespace {

using myframe::TaskExecutor;
using myframe::containers::DoubleBuffer;
using myframe::pipeline::Dag;
using myframe::search::CsrGraph;
using myframe::search::DijkstraOptions;
using myframe::search::DijkstraResult;
using myframe::search::DisjointSet;
using myframe::search::EdgeMergePolicy;
using myframe::search::ShortestPathResult;
using myframe::search::SpanningEdge;
using myframe::search::SpanningForestResult;
using myframe::search::TraversalOptions;
using myframe::search::TraversalWorkspace;
using myframe::search::VertexId;
using myframe::search::VisitAction;
using myframe::search::WeightedCsrGraph;
using myframe::search::kInvalidVertex;

static std::atomic<int> g_checks(0);

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "FAIL [" << __FILE__ << ":" << __LINE__ << "]: "        \
                      << #cond << std::endl;                                     \
            std::exit(1);                                                        \
        }                                                                        \
        g_checks.fetch_add(1, std::memory_order_relaxed);                        \
    } while (0)

#define CHECK_THROWS(expr, ExcType)                                              \
    do {                                                                         \
        bool threw_ = false;                                                     \
        try {                                                                    \
            (void)(expr);                                                        \
        } catch (const ExcType&) {                                               \
            threw_ = true;                                                       \
        }                                                                        \
        CHECK(threw_);                                                           \
    } while (0)

// -----------------------------------------------------------------------------
// 1. DisjointSet (Union-Find)
// -----------------------------------------------------------------------------
void test_disjoint_set() {
    DisjointSet empty_dsu;
    CHECK(empty_dsu.empty());
    CHECK(empty_dsu.num_vertices() == 0);
    CHECK(empty_dsu.num_components() == 0);
    CHECK(empty_dsu.components().empty());
    CHECK_THROWS(empty_dsu.find(0), std::out_of_range);
    CHECK_THROWS(empty_dsu.find_const(0), std::out_of_range);

    DisjointSet dsu(6);
    CHECK(!dsu.empty());
    CHECK(dsu.num_vertices() == 6);
    CHECK(dsu.num_components() == 6);
    CHECK(dsu.unite(0, 1));
    CHECK(!dsu.unite(0, 1));
    CHECK(dsu.unite(1, 2));
    CHECK(dsu.unite(3, 4));
    CHECK(dsu.num_components() == 3);
    CHECK(dsu.connected(0, 2));
    CHECK(dsu.connected_const(0, 2));
    CHECK(!dsu.connected(0, 3));
    CHECK(dsu.component_size(0) == 3);
    CHECK(dsu.component_size_const(2) == 3);
    CHECK(dsu.component_size(3) == 2);
    CHECK(dsu.component_size(5) == 1);

    // Invalid unite must not mutate state.
    CHECK_THROWS(dsu.unite(0, 99), std::out_of_range);
    CHECK_THROWS(dsu.unite(99, 0), std::out_of_range);
    CHECK(dsu.num_components() == 3);

    const std::vector<std::vector<VertexId> > comps = dsu.components();
    CHECK(comps.size() == 3);
    CHECK(comps[0] == std::vector<VertexId>({0, 1, 2}));
    CHECK(comps[1] == std::vector<VertexId>({3, 4}));
    CHECK(comps[2] == std::vector<VertexId>({5}));

    // Dynamic vertex addition
    const VertexId v6 = dsu.add_vertex();
    CHECK(v6 == 6);
    CHECK(dsu.num_vertices() == 7);
    CHECK(dsu.num_components() == 4);
    CHECK(dsu.unite(5, 6));
    CHECK(dsu.num_components() == 3);

    // Move semantics: moved-from is empty and reusable
    DisjointSet moved(std::move(dsu));
    CHECK(dsu.empty());
    CHECK(dsu.num_vertices() == 0);
    CHECK(dsu.num_components() == 0);
    CHECK(moved.num_vertices() == 7);
    CHECK(moved.connected(5, 6));
    dsu.reset(2);
    CHECK(dsu.num_vertices() == 2);
    CHECK(dsu.unite(0, 1));
    CHECK(dsu.num_components() == 1);

    // Deep chain (50,000 vertices) verifies iterative path halving
    {
        const std::size_t kDeep = 50000;
        DisjointSet deep(kDeep);
        for (std::size_t i = 1; i < kDeep; ++i) {
            CHECK(deep.unite(static_cast<VertexId>(i - 1), static_cast<VertexId>(i)));
        }
        CHECK(deep.num_components() == 1);
        CHECK(deep.component_size(0) == kDeep);
        CHECK(deep.connected(0, static_cast<VertexId>(kDeep - 1)));
    }

    // Allocation fault injection on reset() & add_vertex()
    {
        DisjointSet d(3);
        d.unite(0, 1);
        for (std::size_t fail_at = 0; fail_at < 6; ++fail_at) {
            bool threw = false;
            try {
                AllocFaultScope scope(fail_at);
                d.reset(100);
            } catch (const std::bad_alloc&) {
                threw = true;
            }
            if (threw) {
                CHECK(d.num_vertices() == 3);
                CHECK(d.num_components() == 2);
                CHECK(d.connected(0, 1));
                CHECK(!d.connected(0, 2));
            } else {
                CHECK(d.num_vertices() == 100);
                CHECK(d.num_components() == 100);
                d.reset(3);
                d.unite(0, 1);
            }
        }
    }

    {
        DisjointSet d;
        for (std::size_t fail_at = 0; fail_at < 4; ++fail_at) {
            const std::size_t before_n = d.num_vertices();
            const std::size_t before_c = d.num_components();
            bool threw = false;
            try {
                AllocFaultScope scope(fail_at);
                (void)d.add_vertex();
            } catch (const std::bad_alloc&) {
                threw = true;
            }
            if (threw) {
                CHECK(d.num_vertices() == before_n);
                CHECK(d.num_components() == before_c);
            } else {
                CHECK(d.num_vertices() == before_n + 1);
                CHECK(d.num_components() == before_c + 1);
            }
        }
    }
}

// Reference recursive DFS for differential oracle tests on small graphs.
void ref_dfs_helper(const std::vector<std::vector<VertexId> >& adj,
                    VertexId u,
                    std::size_t depth,
                    std::vector<std::uint8_t>& visited,
                    std::vector<std::pair<VertexId, std::size_t> >& order) {
    visited[u] = 1;
    order.push_back(std::make_pair(u, depth));
    for (std::size_t i = 0; i < adj[u].size(); ++i) {
        const VertexId nb = adj[u][i];
        if (!visited[nb]) {
            ref_dfs_helper(adj, nb, depth + 1, visited, order);
        }
    }
}

// -----------------------------------------------------------------------------
// 2. CsrGraph (unweighted CSR, iterative BFS/DFS, shortest hops, workspace)
// -----------------------------------------------------------------------------
void test_csr_graph() {
    // Empty graph
    {
        CsrGraph empty_g;
        CHECK(empty_g.empty());
        CHECK(empty_g.num_vertices() == 0);
        CHECK(empty_g.num_edges() == 0);
        CHECK(!empty_g.has_edge(0, 0));
        CHECK(empty_g.connected_components().empty());
        CHECK_THROWS(empty_g.out_degree(0), std::out_of_range);
        CHECK_THROWS(empty_g.neighbors(0), std::out_of_range);
        std::vector<VertexId> path;
        CHECK_THROWS(empty_g.shortest_path(0, 0, path), std::out_of_range);
        CHECK_THROWS(empty_g.bfs(0, [](VertexId, std::size_t) {}), std::out_of_range);
    }

    // Builder options: sorting, deduplication, self-loops, undirected self-loop
    {
        CsrGraph::Builder b(4);
        CHECK_THROWS(b.add_edge(kInvalidVertex, 0), std::out_of_range);
        CHECK_THROWS(b.add_edge(0, kInvalidVertex), std::out_of_range);
        CHECK_THROWS(b.add_undirected_edge(kInvalidVertex, 1), std::out_of_range);

        b.add_edge(0, 3);
        b.add_edge(0, 1);
        b.add_edge(0, 3); // duplicate
        b.add_undirected_edge(2, 2); // single self-loop, NOT doubled
        b.add_undirected_edge(1, 2);

        // Default: sorted, keep duplicates, allow self-loops
        const CsrGraph g_default = b.build();
        CHECK(g_default.num_vertices() == 4);
        CHECK(g_default.num_edges() == 6);
        CHECK(g_default.is_sorted_neighbors());
        CHECK(g_default.out_degree(0) == 3);
        CHECK(g_default.neighbors(0)[0] == 1);
        CHECK(g_default.neighbors(0)[1] == 3);
        CHECK(g_default.neighbors(0)[2] == 3);
        CHECK(g_default.out_degree(2) == 2); // 1 and 2
        CHECK(g_default.has_edge(0, 3));
        CHECK(g_default.has_edge(2, 2));
        CHECK(!g_default.has_edge(3, 0));
        CHECK(!g_default.has_edge(0, 99));

        // Deduplicate & drop self-loops (builder is non-mutating and reusable)
        CsrGraph::BuildOptions opt;
        opt.deduplicate_edges = true;
        opt.allow_self_loops = false;
        const CsrGraph g_clean = b.build(opt);
        CHECK(g_clean.num_edges() == 4);
        CHECK(g_clean.out_degree(0) == 2);
        CHECK(!g_clean.has_edge(2, 2));
        CHECK(g_clean.has_edge(1, 2) && g_clean.has_edge(2, 1));

        // Unsorted build: has_edge falls back to linear scan
        CsrGraph::BuildOptions opt_unsorted;
        opt_unsorted.sort_neighbors = false;
        opt_unsorted.deduplicate_edges = false;
        const CsrGraph g_unsorted = b.build(opt_unsorted);
        CHECK(!g_unsorted.is_sorted_neighbors());
        CHECK(g_unsorted.neighbors(0)[0] == 3);
        CHECK(g_unsorted.neighbors(0)[1] == 1);
        CHECK(g_unsorted.has_edge(0, 1));
        CHECK(g_unsorted.has_edge(0, 3));
        CHECK(!g_unsorted.has_edge(0, 2));
    }

    // BFS, multi-source BFS, DFS visitor return modes, max_depth, limit, SkipBranch, Stop
    {
        // Tree/DAG:
        //      0 -> {1, 2}
        //      1 -> {3, 4}, 2 -> {5}
        //      3 -> {6}
        CsrGraph::Builder b(7);
        b.add_edge(0, 1);
        b.add_edge(0, 2);
        b.add_edge(1, 3);
        b.add_edge(1, 4);
        b.add_edge(2, 5);
        b.add_edge(3, 6);
        const CsrGraph g = b.build();

        // Void visitor
        std::vector<std::pair<VertexId, std::size_t> > visited;
        const std::size_t cnt = g.bfs(0, [&](VertexId v, std::size_t d) {
            visited.push_back(std::make_pair(v, d));
        });
        CHECK(cnt == 7);
        CHECK(visited.size() == 7);
        CHECK(visited[0] == std::make_pair(VertexId(0), std::size_t(0)));
        CHECK(visited[1] == std::make_pair(VertexId(1), std::size_t(1)));
        CHECK(visited[2] == std::make_pair(VertexId(2), std::size_t(1)));
        CHECK(visited[3] == std::make_pair(VertexId(3), std::size_t(2)));
        CHECK(visited[4] == std::make_pair(VertexId(4), std::size_t(2)));
        CHECK(visited[5] == std::make_pair(VertexId(5), std::size_t(2)));
        CHECK(visited[6] == std::make_pair(VertexId(6), std::size_t(3)));

        // Bool visitor early stop
        std::vector<VertexId> stopped;
        const std::size_t stop_cnt = g.bfs(0, [&](VertexId v, std::size_t) -> bool {
            stopped.push_back(v);
            return v != 2;
        });
        CHECK(stop_cnt == 3);
        CHECK(stopped == std::vector<VertexId>({0, 1, 2}));

        // VisitAction::SkipBranch skips expanding subtree of 1 (so 3, 4, 6 not visited)
        std::vector<VertexId> pruned;
        g.bfs(0, [&](VertexId v, std::size_t) -> VisitAction {
            pruned.push_back(v);
            return (v == 1) ? VisitAction::SkipBranch : VisitAction::Continue;
        });
        CHECK(pruned == std::vector<VertexId>({0, 1, 2, 5}));

        // max_depth and limit
        TraversalOptions opts;
        opts.max_depth = 1;
        std::vector<VertexId> d1;
        CHECK(g.bfs(0, [&](VertexId v, std::size_t) { d1.push_back(v); }, opts) == 3);
        CHECK(d1 == std::vector<VertexId>({0, 1, 2}));

        opts.max_depth = 10;
        opts.limit = 4;
        std::vector<VertexId> l4;
        CHECK(g.bfs(0, [&](VertexId v, std::size_t) { l4.push_back(v); }, opts) == 4);
        CHECK(l4 == std::vector<VertexId>({0, 1, 2, 3}));

        opts.limit = 0;
        CHECK(g.bfs(0, [&](VertexId, std::size_t) { CHECK(false); }, opts) == 0);
        CHECK_THROWS(g.bfs(99, [&](VertexId, std::size_t) {}, opts), std::out_of_range);

        // Multi-source BFS with duplicates and invalid-source validation
        std::vector<std::pair<VertexId, std::size_t> > ms;
        const std::vector<VertexId> srcs = {2, 1, 2};
        CHECK(g.multi_source_bfs(srcs, [&](VertexId v, std::size_t d) {
            ms.push_back(std::make_pair(v, d));
        }) == 6);
        CHECK(ms[0] == std::make_pair(VertexId(2), std::size_t(0)));
        CHECK(ms[1] == std::make_pair(VertexId(1), std::size_t(0)));
        CHECK(ms[2] == std::make_pair(VertexId(5), std::size_t(1)));

        const std::vector<VertexId> bad_srcs = {1, 99};
        CHECK_THROWS(g.multi_source_bfs(bad_srcs, [](VertexId, std::size_t) {
            CHECK(false);
        }), std::out_of_range);

        // Iterative DFS pre-order + SkipBranch + max_depth + limit + root Stop/SkipBranch
        std::vector<VertexId> dfs_order;
        CHECK(g.dfs(0, [&](VertexId v, std::size_t) { dfs_order.push_back(v); }) == 7);
        CHECK(dfs_order == std::vector<VertexId>({0, 1, 3, 6, 4, 2, 5}));

        std::vector<VertexId> dfs_pruned;
        g.dfs(0, [&](VertexId v, std::size_t) -> VisitAction {
            dfs_pruned.push_back(v);
            return (v == 1) ? VisitAction::SkipBranch : VisitAction::Continue;
        });
        CHECK(dfs_pruned == std::vector<VertexId>({0, 1, 2, 5}));

        TraversalOptions dfs_opts;
        dfs_opts.max_depth = 1;
        std::vector<VertexId> dfs_d1;
        CHECK(g.dfs(0, [&](VertexId v, std::size_t) { dfs_d1.push_back(v); }, dfs_opts) == 3);
        CHECK(dfs_d1 == std::vector<VertexId>({0, 1, 2}));

        dfs_opts.max_depth = 10;
        dfs_opts.limit = 4;
        std::vector<VertexId> dfs_l4;
        CHECK(g.dfs(0, [&](VertexId v, std::size_t) { dfs_l4.push_back(v); }, dfs_opts) == 4);
        CHECK(dfs_l4 == std::vector<VertexId>({0, 1, 3, 6}));

        // Root Stop / SkipBranch
        std::vector<VertexId> root_only;
        CHECK(g.dfs(0, [&](VertexId v, std::size_t) -> VisitAction {
            root_only.push_back(v);
            return VisitAction::SkipBranch;
        }) == 1);
        CHECK(root_only == std::vector<VertexId>({0}));

        // Multi-source BFS with duplicates and small limit
        {
            TraversalOptions lim2;
            lim2.limit = 2;
            std::vector<VertexId> ms_lim;
            const std::vector<VertexId> dup_srcs = {2, 2, 1, 0};
            CHECK(g.multi_source_bfs(dup_srcs, [&](VertexId v, std::size_t) {
                ms_lim.push_back(v);
            }, lim2) == 2);
            CHECK(ms_lim == std::vector<VertexId>({2, 1}));
        }

        // Visitor throwing exception leaves TraversalWorkspace reusable
        {
            TraversalWorkspace ex_ws;
            bool threw = false;
            try {
                g.bfs(0, [](VertexId v, std::size_t) {
                    if (v == 2) {
                        throw std::runtime_error("visitor-abort");
                    }
                }, ex_ws);
            } catch (const std::runtime_error&) {
                threw = true;
            }
            CHECK(threw);
            std::size_t after = 0;
            CHECK(g.bfs(0, [&](VertexId, std::size_t) { ++after; }, ex_ws) == 7);
            CHECK(after == 7);
        }

        // Shortest path & reachability
        std::vector<VertexId> sp;
        CHECK(g.shortest_path(0, 6, sp));
        CHECK(sp == std::vector<VertexId>({0, 1, 3, 6}));
        CHECK(!g.shortest_path(0, 6, sp, 2));
        CHECK(sp.empty());
        CHECK(g.shortest_path(4, 4, sp, 0));
        CHECK(sp == std::vector<VertexId>({4}));
        CHECK(g.is_reachable(0, 6, 3));
        CHECK(!g.is_reachable(0, 6, 2));
        CHECK(!g.is_reachable(5, 0));

        // TraversalWorkspace zero-allocation steady-state verification
        TraversalWorkspace ws;
        ws.reserve(g.num_vertices(), g.num_vertices());
        CHECK(ws.vertex_capacity() >= g.num_vertices());
        {
            AllocFaultScope count_scope(std::numeric_limits<std::size_t>::max());
            for (int iter = 0; iter < 100; ++iter) {
                std::size_t seen_b = 0;
                std::size_t seen_d = 0;
                g.bfs(0, [&](VertexId, std::size_t) { ++seen_b; }, ws);
                g.dfs(0, [&](VertexId, std::size_t) { ++seen_d; }, ws);
                CHECK(seen_b == 7);
                CHECK(seen_d == 7);
            }
            CHECK(count_scope.calls() == 0);
        }
    }

    // Deep chain (60,000 vertices): proves BFS/DFS/shortest_path are non-recursive
    {
        const std::size_t kDeep = 60000;
        CsrGraph::Builder b(kDeep);
        b.reserve_edges(kDeep - 1);
        for (std::size_t i = 0; i + 1 < kDeep; ++i) {
            b.add_edge(static_cast<VertexId>(i), static_cast<VertexId>(i + 1));
        }
        const CsrGraph deep = b.build();

        std::size_t last_depth_bfs = 0;
        CHECK(deep.bfs(0, [&](VertexId, std::size_t d) { last_depth_bfs = d; }) == kDeep);
        CHECK(last_depth_bfs == kDeep - 1);

        std::size_t last_depth_dfs = 0;
        CHECK(deep.dfs(0, [&](VertexId, std::size_t d) { last_depth_dfs = d; }) == kDeep);
        CHECK(last_depth_dfs == kDeep - 1);

        std::vector<VertexId> path;
        CHECK(deep.shortest_path(0, static_cast<VertexId>(kDeep - 1), path));
        CHECK(path.size() == kDeep);
        CHECK(path.front() == 0 && path.back() == static_cast<VertexId>(kDeep - 1));
    }

    // Weakly connected components, strongly connected components (Tarjan), & move safety
    {
        CsrGraph::Builder b(6);
        b.add_edge(0, 1);
        b.add_edge(2, 1); // directed into 1, same weak component {0, 1, 2}
        b.add_edge(3, 4);
        CsrGraph g = b.build();
        const std::vector<std::vector<VertexId> > comps = g.connected_components();
        CHECK(comps.size() == 3);
        CHECK(comps[0] == std::vector<VertexId>({0, 1, 2}));
        CHECK(comps[1] == std::vector<VertexId>({3, 4}));
        CHECK(comps[2] == std::vector<VertexId>({5}));

        // Iterative Tarjan SCC test
        CsrGraph::Builder sb(7);
        sb.add_edge(0, 1);
        sb.add_edge(1, 2);
        sb.add_edge(2, 0); // SCC {0, 1, 2}
        sb.add_edge(2, 3);
        sb.add_edge(3, 4);
        sb.add_edge(4, 3); // SCC {3, 4}
        sb.add_edge(4, 5); // SCC {5}
        // 6 isolated -> SCC {6}
        const CsrGraph sg = sb.build();
        const std::vector<std::vector<VertexId> > sccs = sg.strongly_connected_components();
        CHECK(sccs.size() == 4);
        // Verify reverse condensation topological order: for any edge u -> v
        // between distinct SCCs, scc_index[u] > scc_index[v].
        std::vector<std::size_t> v_to_scc(sg.num_vertices(), 999);
        for (std::size_t ci = 0; ci < sccs.size(); ++ci) {
            for (std::size_t j = 0; j < sccs[ci].size(); ++j) {
                v_to_scc[sccs[ci][j]] = ci;
            }
        }
        for (std::size_t u = 0; u < sg.num_vertices(); ++u) {
            const myframe::search::VertexSpan nbs = sg.neighbors(static_cast<VertexId>(u));
            for (std::size_t j = 0; j < nbs.size(); ++j) {
                CHECK(v_to_scc[u] >= v_to_scc[nbs[j]]);
            }
        }
        std::set<std::vector<VertexId> > scc_set(sccs.begin(), sccs.end());
        CHECK(scc_set.count(std::vector<VertexId>({0, 1, 2})) == 1);
        CHECK(scc_set.count(std::vector<VertexId>({3, 4})) == 1);
        CHECK(scc_set.count(std::vector<VertexId>({5})) == 1);
        CHECK(scc_set.count(std::vector<VertexId>({6})) == 1);

        // Deep 50,000-node cycle SCC (proves Tarjan is 100% iterative)
        const std::size_t kDeepScc = 50000;
        CsrGraph::Builder dsb(kDeepScc);
        dsb.reserve_edges(kDeepScc);
        for (std::size_t i = 0; i < kDeepScc; ++i) {
            dsb.add_edge(static_cast<VertexId>(i),
                         static_cast<VertexId>((i + 1) % kDeepScc));
        }
        const CsrGraph deep_scc_g = dsb.build();
        const std::vector<std::vector<VertexId> > deep_sccs =
            deep_scc_g.strongly_connected_components();
        CHECK(deep_sccs.size() == 1 && deep_sccs[0].size() == kDeepScc);

        CsrGraph moved(std::move(g));
        CHECK(g.empty());
        CHECK(g.num_vertices() == 0);
        CHECK(g.num_edges() == 0);
        CHECK(!g.has_edge(0, 1));
        CHECK(moved.num_vertices() == 6);
        CHECK(moved.has_edge(0, 1));
    }

    // Randomized differential oracle test against reference BFS & recursive DFS
    {
        std::mt19937 rng(20261126U);
        for (int trial = 0; trial < 50; ++trial) {
            const std::size_t n = 1 + (rng() % 35);
            const std::size_t m = rng() % (n * 4 + 1);
            CsrGraph::Builder b(n);
            std::vector<std::vector<VertexId> > ref_adj(n);
            for (std::size_t e = 0; e < m; ++e) {
                const VertexId u = static_cast<VertexId>(rng() % n);
                const VertexId v = static_cast<VertexId>(rng() % n);
                b.add_edge(u, v);
                ref_adj[u].push_back(v);
            }
            for (std::size_t u = 0; u < n; ++u) {
                std::sort(ref_adj[u].begin(), ref_adj[u].end());
            }
            const CsrGraph g = b.build();

            const VertexId src = static_cast<VertexId>(rng() % n);

            // Reference BFS distances
            std::vector<int> ref_dist(n, -1);
            std::queue<VertexId> q;
            ref_dist[src] = 0;
            q.push(src);
            while (!q.empty()) {
                const VertexId u = q.front();
                q.pop();
                for (std::size_t i = 0; i < ref_adj[u].size(); ++i) {
                    const VertexId nb = ref_adj[u][i];
                    if (ref_dist[nb] == -1) {
                        ref_dist[nb] = ref_dist[u] + 1;
                        q.push(nb);
                    }
                }
            }

            std::vector<int> got_dist(n, -1);
            g.bfs(src, [&](VertexId v, std::size_t d) {
                CHECK(got_dist[v] == -1);
                got_dist[v] = static_cast<int>(d);
            });
            CHECK(got_dist == ref_dist);

            // Reference recursive DFS order vs iterative CsrGraph::dfs
            std::vector<std::uint8_t> ref_vis(n, 0);
            std::vector<std::pair<VertexId, std::size_t> > ref_dfs;
            ref_dfs_helper(ref_adj, src, 0, ref_vis, ref_dfs);

            std::vector<std::pair<VertexId, std::size_t> > got_dfs;
            g.dfs(src, [&](VertexId v, std::size_t d) {
                got_dfs.push_back(std::make_pair(v, d));
            });
            CHECK(got_dfs == ref_dfs);

            // Verify shortest_path hop length matches ref_dist
            const VertexId dst = static_cast<VertexId>(rng() % n);
            std::vector<VertexId> sp;
            const bool found = g.shortest_path(src, dst, sp);
            CHECK(found == (ref_dist[dst] != -1));
            if (found) {
                CHECK(sp.size() == static_cast<std::size_t>(ref_dist[dst]) + 1);
                CHECK(sp.front() == src && sp.back() == dst);
                for (std::size_t i = 0; i + 1 < sp.size(); ++i) {
                    CHECK(g.has_edge(sp[i], sp[i + 1]));
                }
            }
        }
    }
}

// -----------------------------------------------------------------------------
// 3. WeightedCsrGraph & Dijkstra (double, int, uint64_t, edge policies, overflow)
// -----------------------------------------------------------------------------
void test_weighted_csr_graph_and_dijkstra() {
    // Reject non-finite weights at build time
    {
        WeightedCsrGraph<double>::Builder b(3);
        CHECK_THROWS(b.add_edge(0, 1, std::numeric_limits<double>::quiet_NaN()),
                     std::invalid_argument);
        CHECK_THROWS(b.add_edge(0, 1, std::numeric_limits<double>::infinity()),
                     std::invalid_argument);
        CHECK_THROWS(b.add_undirected_edge(0, 1, -std::numeric_limits<double>::infinity()),
                     std::invalid_argument);
    }

    // Parallel edge merge policies: KeepAll, KeepMinWeight, KeepMaxWeight, KeepLast
    {
        WeightedCsrGraph<double>::Builder b(3);
        b.add_edge(0, 1, 5.0);
        b.add_edge(0, 1, 2.0);
        b.add_edge(0, 1, 8.0);
        b.add_edge(1, 2, 1.5);

        const WeightedCsrGraph<double> g_all = b.build();
        CHECK(g_all.num_edges() == 4);
        CHECK(g_all.out_degree(0) == 3);
        // Dijkstra handles parallel edges cleanly under KeepAll
        std::vector<VertexId> path;
        double dist = 0.0;
        CHECK(g_all.shortest_path(0, 2, path, &dist));
        CHECK(path == std::vector<VertexId>({0, 1, 2}));
        CHECK(std::fabs(dist - 3.5) < 1e-12);

        WeightedCsrGraph<double>::BuildOptions opt;
        opt.merge_policy = EdgeMergePolicy::KeepMinWeight;
        const WeightedCsrGraph<double> g_min = b.build(opt);
        CHECK(g_min.num_edges() == 2);
        CHECK(g_min.weights(0).size() == 1);
        CHECK(std::fabs(g_min.weights(0)[0] - 2.0) < 1e-12);

        opt.merge_policy = EdgeMergePolicy::KeepMaxWeight;
        const WeightedCsrGraph<double> g_max = b.build(opt);
        CHECK(std::fabs(g_max.weights(0)[0] - 8.0) < 1e-12);

        opt.merge_policy = EdgeMergePolicy::KeepLast;
        const WeightedCsrGraph<double> g_last = b.build(opt);
        CHECK(std::fabs(g_last.weights(0)[0] - 8.0) < 1e-12);
    }

    // Negative finite weights allowed in graph (usable for BFS/DFS), rejected by Dijkstra
    {
        WeightedCsrGraph<int>::Builder b(3);
        b.add_edge(0, 1, -3);
        b.add_edge(1, 2, 5);
        const WeightedCsrGraph<int> g = b.build();
        CHECK(g.has_negative_weights());
        std::vector<std::pair<VertexId, int> > edges;
        g.for_each_edge(0, [&](VertexId to, int w) {
            edges.push_back(std::make_pair(to, w));
        });
        CHECK(edges.size() == 1 && edges[0].first == 1 && edges[0].second == -3);
        CHECK(g.is_reachable(0, 2));
        CHECK_THROWS(g.dijkstra(0), std::domain_error);
    }

    // Dijkstra options: zero-weight edges, max_distance, stop_at_target, invalid opts
    {
        WeightedCsrGraph<double>::Builder b(6);
        b.add_edge(0, 1, 0.0);
        b.add_edge(1, 2, 2.5);
        b.add_edge(0, 2, 4.0);
        b.add_edge(2, 3, 3.0);
        b.add_edge(3, 4, 1.0);
        // 5 is unreachable
        const WeightedCsrGraph<double> g = b.build();

        DijkstraOptions<double> bad_opt;
        bad_opt.has_max_distance = true;
        bad_opt.max_distance = -1.0;
        CHECK_THROWS(g.dijkstra(0, bad_opt), std::invalid_argument);
        bad_opt.max_distance = std::numeric_limits<double>::quiet_NaN();
        CHECK_THROWS(g.dijkstra(0, bad_opt), std::invalid_argument);
        bad_opt.max_distance = std::numeric_limits<double>::infinity();
        CHECK_THROWS(g.dijkstra(0, bad_opt), std::invalid_argument);

        const DijkstraResult<double> full = g.dijkstra(0);
        CHECK(full.is_reachable(0) && full.distance_to(0) == 0.0);
        CHECK(full.is_reachable(1) && full.distance_to(1) == 0.0);
        CHECK(full.is_reachable(2) && std::fabs(full.distance_to(2) - 2.5) < 1e-12);
        CHECK(full.is_reachable(4) && std::fabs(full.distance_to(4) - 6.5) < 1e-12);
        CHECK(!full.is_reachable(5));
        CHECK_THROWS(full.distance_to(5), std::out_of_range);

        std::vector<VertexId> path;
        CHECK(full.reconstruct_path(4, path));
        CHECK(path == std::vector<VertexId>({0, 1, 2, 3, 4}));
        CHECK(!full.reconstruct_path(5, path));
        CHECK(path.empty());

        // max_distance cutoff
        double d = 0.0;
        CHECK(g.shortest_path(0, 3, 5.5, path, &d));
        CHECK(std::fabs(d - 5.5) < 1e-12);
        CHECK(!g.shortest_path(0, 4, 5.5, path, &d));
        CHECK(path.empty());

        // Multi-source Dijkstra: nearest source & path reconstruction
        const std::vector<VertexId> empty_srcs;
        const ShortestPathResult<double> empty_ms = g.multi_source_dijkstra(empty_srcs);
        CHECK(!empty_ms.is_reachable(0));

        const std::vector<VertexId> ms_srcs = {0, 3, 3};
        const ShortestPathResult<double> ms = g.multi_source_dijkstra(ms_srcs);
        CHECK(ms.source == kInvalidVertex);
        CHECK(ms.is_reachable(2) && ms.nearest_source(2) == 0);
        CHECK(std::fabs(ms.distance_to(2) - 2.5) < 1e-12);
        CHECK(ms.is_reachable(4) && ms.nearest_source(4) == 3);
        CHECK(std::fabs(ms.distance_to(4) - 1.0) < 1e-12);
        CHECK(ms.reconstruct_path(4, path));
        CHECK(path == std::vector<VertexId>({3, 4}));
        CHECK(!ms.is_reachable(5));
        CHECK_THROWS(ms.nearest_source(5), std::out_of_range);
    }

    // A* heuristic shortest path: 2D grid + inconsistent-but-admissible re-opening + errors
    {
        const int kW = 10;
        const int kH = 10;
        auto vid = [&](int x, int y) -> VertexId {
            return static_cast<VertexId>(y * kW + x);
        };
        WeightedCsrGraph<int>::Builder gb(static_cast<std::size_t>(kW * kH));
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                // Wall at x == 5 except opening at y == 8
                if (x == 5 && y != 8) {
                    continue;
                }
                const int dx[4] = {1, -1, 0, 0};
                const int dy[4] = {0, 0, 1, -1};
                for (int dir = 0; dir < 4; ++dir) {
                    const int nx = x + dx[dir];
                    const int ny = y + dy[dir];
                    if (nx < 0 || nx >= kW || ny < 0 || ny >= kH) {
                        continue;
                    }
                    if (nx == 5 && ny != 8) {
                        continue;
                    }
                    gb.add_edge(vid(x, y), vid(nx, ny), 1);
                }
            }
        }
        const WeightedCsrGraph<int> grid = gb.build();
        const VertexId src = vid(0, 0);
        const VertexId dst = vid(9, 1);

        std::vector<VertexId> dijk_path;
        int dijk_dist = 0;
        CHECK(grid.shortest_path(src, dst, dijk_path, &dijk_dist));

        std::vector<VertexId> astar_path;
        int astar_dist = 0;
        auto manhattan = [&](VertexId v) -> int {
            const int x = static_cast<int>(v) % kW;
            const int y = static_cast<int>(v) / kW;
            return std::abs(x - 9) + std::abs(y - 1);
        };
        CHECK(grid.astar(src, dst, manhattan, astar_path, &astar_dist));
        CHECK(astar_dist == dijk_dist);
        CHECK(astar_path.front() == src && astar_path.back() == dst);
        CHECK(astar_path.size() == static_cast<std::size_t>(astar_dist) + 1);

        // Max distance bound in A*
        CHECK(grid.astar(src, dst, dijk_dist, manhattan, astar_path, &astar_dist));
        CHECK(!grid.astar(src, dst, dijk_dist - 1, manhattan, astar_path, &astar_dist));
        CHECK(astar_path.empty());

        // Admissible but inconsistent heuristic: verifies re-opening produces optimal path!
        // 0 -> 1 (1), 0 -> 2 (2), 2 -> 1 (1 -- unused), 1 -> 3 (10), 2 -> 3 (1)
        // Better classic inconsistent example:
        // 0 -> 1 (1), 0 -> 2 (5), 1 -> 2 (1), 2 -> 3 (10)
        // h(0)=0, h(1)=10 (<= true 11), h(2)=0 (<= true 10), h(3)=0.
        // Pop 0 -> push 1 (f=1+10=11, g=1), 2 (f=5+0=5, g=5).
        // Pop 2 first (g=5), pushes 3 (f=15, g=15).
        // Pop 1 (f=11, g=1), improves 2 to g=2 (f=2) -> re-opens 2 -> improves 3 to g=12!
        WeightedCsrGraph<int>::Builder ib(4);
        ib.add_edge(0, 1, 1);
        ib.add_edge(0, 2, 5);
        ib.add_edge(1, 2, 1);
        ib.add_edge(2, 3, 10);
        const WeightedCsrGraph<int> ig = ib.build();
        const int h_inconsistent[4] = {0, 10, 0, 0};
        std::vector<VertexId> ipath;
        int idist = 0;
        CHECK(ig.astar(0, 3, [&](VertexId v) { return h_inconsistent[v]; }, ipath, &idist));
        CHECK(idist == 12);
        CHECK(ipath == std::vector<VertexId>({0, 1, 2, 3}));

        // Invalid heuristic rejected
        CHECK_THROWS(ig.astar(0, 3, [](VertexId) { return -1; }, ipath, &idist),
                     std::invalid_argument);
    }

    // Bellman-Ford: negative weights, reachable negative cycle, n == 1 negative self-loop,
    // unreachable negative cycle, zero-weight cycle, and signed overflow/underflow!
    {
        // Valid negative-weight edges (no negative cycle)
        WeightedCsrGraph<int>::Builder b(5);
        b.add_edge(0, 1, 6);
        b.add_edge(0, 2, 7);
        b.add_edge(1, 2, 8);
        b.add_edge(1, 3, 5);
        b.add_edge(1, 4, -4);
        b.add_edge(2, 3, -3);
        b.add_edge(2, 4, 9);
        b.add_edge(3, 1, -2);
        b.add_edge(4, 0, 2);
        b.add_edge(4, 3, 7);
        const WeightedCsrGraph<int> g = b.build();

        ShortestPathResult<int> bf;
        CHECK(g.bellman_ford(0, bf));
        CHECK(bf.distance_to(0) == 0);
        CHECK(bf.distance_to(1) == 2);
        CHECK(bf.distance_to(2) == 7);
        CHECK(bf.distance_to(3) == 4);
        CHECK(bf.distance_to(4) == -2);
        std::vector<VertexId> path;
        CHECK(bf.reconstruct_path(4, path));
        CHECK(path == std::vector<VertexId>({0, 2, 3, 1, 4}));

        // Reachable negative cycle
        WeightedCsrGraph<int>::Builder nb(4);
        nb.add_edge(0, 1, 1);
        nb.add_edge(1, 2, -2);
        nb.add_edge(2, 1, -2);
        nb.add_edge(2, 3, 1);
        const WeightedCsrGraph<int> ng = nb.build();
        CHECK(!ng.bellman_ford(0, bf));
        CHECK(bf.reached.empty());

        // Single-vertex negative self-loop
        WeightedCsrGraph<int>::Builder sb(1);
        sb.add_edge(0, 0, -1);
        const WeightedCsrGraph<int> sg = sb.build();
        CHECK(!sg.bellman_ford(0, bf));

        // Unreachable negative cycle does NOT invalidate reachable shortest paths from 0
        WeightedCsrGraph<int>::Builder ub(4);
        ub.add_edge(0, 1, 3);
        ub.add_edge(2, 3, -1);
        ub.add_edge(3, 2, -1);
        const WeightedCsrGraph<int> ug = ub.build();
        CHECK(ug.bellman_ford(0, bf));
        CHECK(bf.is_reachable(1) && bf.distance_to(1) == 3);
        CHECK(!bf.is_reachable(2));

        // Zero-weight cycle terminates cleanly
        WeightedCsrGraph<int>::Builder zb(3);
        zb.add_edge(0, 1, 2);
        zb.add_edge(1, 2, -2);
        zb.add_edge(2, 0, 0);
        const WeightedCsrGraph<int> zg = zb.build();
        CHECK(zg.bellman_ford(0, bf));
        CHECK(bf.distance_to(1) == 2 && bf.distance_to(2) == 0);

        // Signed underflow protection
        WeightedCsrGraph<int>::Builder ob(3);
        const int kIntMin = std::numeric_limits<int>::min();
        ob.add_edge(0, 1, kIntMin + 5);
        ob.add_edge(1, 2, -10);
        const WeightedCsrGraph<int> og = ob.build();
        CHECK_THROWS(og.bellman_ford(0, bf), std::overflow_error);
    }

    // Kruskal Minimum Spanning Forest (connected, disconnected, negative weights, overflow)
    {
        WeightedCsrGraph<int>::Builder b(6);
        b.add_undirected_edge(0, 1, 4);
        b.add_undirected_edge(0, 2, 1);
        b.add_undirected_edge(1, 2, 2);
        b.add_undirected_edge(1, 3, 5);
        b.add_undirected_edge(2, 3, 3);
        b.add_edge(0, 0, -100); // self-loop must be ignored
        // Component 2: {4, 5} with negative weight
        b.add_undirected_edge(4, 5, -7);
        const WeightedCsrGraph<int> g = b.build();

        const SpanningForestResult<int> msf = g.minimum_spanning_forest();
        CHECK(msf.num_components == 2);
        CHECK(msf.edges.size() == 4); // 3 edges in {0,1,2,3} + 1 edge in {4,5}
        // {0,1,2,3} MST = 1 + 2 + 3 = 6; {4,5} = -7; total = -1
        CHECK(msf.total_weight == -1);

        // Total weight overflow guard
        WeightedCsrGraph<std::uint64_t>::Builder ob(3);
        const std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
        ob.add_undirected_edge(0, 1, kMax - 1);
        ob.add_undirected_edge(1, 2, 10);
        const WeightedCsrGraph<std::uint64_t> og = ob.build();
        CHECK_THROWS(og.minimum_spanning_forest(), std::overflow_error);
    }

    // Distance overflow protection (uint64_t and double) + overflow-safe max_distance pruning
    {
        WeightedCsrGraph<std::uint64_t>::Builder b(4);
        const std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
        b.add_edge(0, 1, 10);
        b.add_edge(1, 2, kMax - 5); // 10 + (UINT64_MAX - 5) overflows uint64_t
        b.add_edge(1, 3, 20);
        const WeightedCsrGraph<std::uint64_t> g = b.build();

        // Unbounded Dijkstra must throw std::overflow_error (no silent wraparound!)
        CHECK_THROWS(g.dijkstra(0), std::overflow_error);

        // With max_distance = 100, the huge edge (kMax - 5) exceeds max_distance - 10
        // and must be pruned cleanly without throwing overflow!
        std::vector<VertexId> path;
        std::uint64_t dist = 0;
        CHECK(g.shortest_path(0, 3, std::uint64_t(100), path, &dist));
        CHECK(dist == 30);
        CHECK(path == std::vector<VertexId>({0, 1, 3}));
        CHECK(!g.shortest_path(0, 2, std::uint64_t(100), path, &dist));

        // Double overflow
        WeightedCsrGraph<double>::Builder bd(3);
        const double kDblMax = std::numeric_limits<double>::max();
        bd.add_edge(0, 1, kDblMax);
        bd.add_edge(1, 2, kDblMax);
        const WeightedCsrGraph<double> gd = bd.build();
        CHECK_THROWS(gd.dijkstra(0), std::overflow_error);
    }

    // Move safety
    {
        WeightedCsrGraph<std::uint64_t>::Builder b(2);
        b.add_edge(0, 1, 42);
        WeightedCsrGraph<std::uint64_t> g = b.build();
        WeightedCsrGraph<std::uint64_t> moved(std::move(g));
        CHECK(g.empty() && g.num_vertices() == 0 && g.num_edges() == 0);
        CHECK(moved.num_vertices() == 2 && moved.num_edges() == 1);
    }

    // Randomized differential oracle test against Bellman-Ford (uint64_t)
    {
        std::mt19937 rng(987654321U);
        for (int trial = 0; trial < 40; ++trial) {
            const std::size_t n = 1 + (rng() % 30);
            const std::size_t m = rng() % (n * 4 + 1);
            WeightedCsrGraph<std::uint64_t>::Builder b(n);
            struct RefEdge {
                VertexId u;
                VertexId v;
                std::uint64_t w;
            };
            std::vector<RefEdge> ref_edges;
            for (std::size_t e = 0; e < m; ++e) {
                const VertexId u = static_cast<VertexId>(rng() % n);
                const VertexId v = static_cast<VertexId>(rng() % n);
                const std::uint64_t w = rng() % 50;
                b.add_edge(u, v, w);
                ref_edges.push_back(RefEdge{u, v, w});
            }
            const WeightedCsrGraph<std::uint64_t> g = b.build();
            const VertexId src = static_cast<VertexId>(rng() % n);

            // Bellman-Ford oracle
            const std::uint64_t kInf = std::numeric_limits<std::uint64_t>::max();
            std::vector<std::uint64_t> ref_dist(n, kInf);
            ref_dist[src] = 0;
            for (std::size_t iter = 0; iter + 1 < n; ++iter) {
                bool changed = false;
                for (std::size_t e = 0; e < ref_edges.size(); ++e) {
                    const RefEdge& re = ref_edges[e];
                    if (ref_dist[re.u] != kInf && ref_dist[re.u] + re.w < ref_dist[re.v]) {
                        ref_dist[re.v] = ref_dist[re.u] + re.w;
                        changed = true;
                    }
                }
                if (!changed) {
                    break;
                }
            }

            const DijkstraResult<std::uint64_t> res = g.dijkstra(src);
            for (std::size_t v = 0; v < n; ++v) {
                const VertexId vid = static_cast<VertexId>(v);
                CHECK(res.is_reachable(vid) == (ref_dist[v] != kInf));
                if (ref_dist[v] != kInf) {
                    CHECK(res.distance_to(vid) == ref_dist[v]);
                    std::vector<VertexId> path;
                    CHECK(res.reconstruct_path(vid, path));
                    CHECK(!path.empty() && path.front() == src && path.back() == vid);
                }
            }
        }
    }
}

// -----------------------------------------------------------------------------
// 4. DoubleBuffer<CsrGraph> & DoubleBuffer<WeightedCsrGraph> hot-reload stress
// -----------------------------------------------------------------------------
void test_double_buffer_graph_hot_reload() {
    auto build_ring = [](std::size_t n) -> CsrGraph {
        CsrGraph::Builder b(n);
        for (std::size_t i = 0; i < n; ++i) {
            b.add_edge(static_cast<VertexId>(i),
                       static_cast<VertexId>((i + 1) % n));
        }
        return b.build();
    };

    DoubleBuffer<CsrGraph> db(build_ring(16));
    std::atomic<bool> stop(false);
    std::atomic<std::size_t> total_queries(0);

    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&]() {
            TraversalWorkspace ws;
            while (!stop.load(std::memory_order_acquire)) {
                auto guard = db.read();
                const CsrGraph& g = *guard;
                const std::size_t n = g.num_vertices();
                CHECK(n >= 8 && n <= 64);
                std::size_t count = g.bfs(0, [](VertexId, std::size_t) {}, ws);
                CHECK(count == n);
                total_queries.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (std::size_t gen = 0; gen < 80; ++gen) {
        const std::size_t next_n = 8 + (gen % 57);
        db.publish(build_ring(next_n));
        if ((gen & 7U) == 0) {
            std::this_thread::yield();
        }
    }
    stop.store(true, std::memory_order_release);
    for (std::size_t i = 0; i < readers.size(); ++i) {
        readers[i].join();
    }
    CHECK(total_queries.load(std::memory_order_relaxed) > 0);
}

// -----------------------------------------------------------------------------
// 5. Dag (topological sort/layers, cycle detection, parallel & executor runner)
// -----------------------------------------------------------------------------
void test_dag() {
    // Empty DAG
    {
        Dag<std::string> empty_dag;
        CHECK(empty_dag.empty());
        CHECK(empty_dag.num_nodes() == 0);
        CHECK(empty_dag.num_edges() == 0);
        CHECK(!empty_dag.has_cycle());
        std::vector<std::string> order;
        std::vector<std::vector<std::string> > layers;
        std::vector<std::string> cycle;
        CHECK(!empty_dag.find_cycle(cycle));
        CHECK(empty_dag.topological_sort(order) && order.empty());
        CHECK(empty_dag.topological_layers(layers) && layers.empty());
        empty_dag.execute_parallel(4, [](const std::string&) { CHECK(false); });
        CHECK_THROWS(empty_dag.prerequisites("missing"), std::out_of_range);
    }

    // Serving pipeline DAG: topological_sort, topological_layers, ancestors, descendants
    {
        Dag<std::string> dag;
        CHECK(dag.add_node("query_parse"));
        CHECK(!dag.add_node("query_parse")); // idempotent

        // query_parse -> {user_profile, recall_inv, recall_graph} -> merge -> rank -> rerank
        CHECK(dag.add_edge("query_parse", "user_profile"));
        CHECK(!dag.add_edge("query_parse", "user_profile")); // idempotent
        CHECK(dag.add_dependency("recall_inv", "query_parse"));
        CHECK(dag.add_dependency("recall_graph", "query_parse"));
        CHECK(dag.add_edge("user_profile", "merge"));
        CHECK(dag.add_edge("recall_inv", "merge"));
        CHECK(dag.add_edge("recall_graph", "merge"));
        CHECK(dag.add_edge("merge", "rank"));
        CHECK(dag.add_edge("rank", "rerank"));
        CHECK(dag.add_node("isolated_metrics"));

        CHECK(dag.num_nodes() == 8);
        CHECK(dag.num_edges() == 8);
        CHECK(dag.contains("merge"));
        CHECK(dag.has_edge("query_parse", "recall_inv"));
        CHECK(!dag.has_edge("recall_inv", "query_parse"));
        CHECK(dag.in_degree("merge") == 3);
        CHECK(dag.out_degree("query_parse") == 3);

        const std::vector<std::string> anc = dag.ancestors("rank");
        const std::set<std::string> anc_set(anc.begin(), anc.end());
        CHECK(anc_set == std::set<std::string>(
            {"query_parse", "user_profile", "recall_inv", "recall_graph", "merge"}));

        const std::vector<std::string> desc = dag.descendants("recall_inv");
        const std::set<std::string> desc_set(desc.begin(), desc.end());
        CHECK(desc_set == std::set<std::string>({"merge", "rank", "rerank"}));

        CHECK(!dag.has_cycle());
        std::vector<std::string> cycle;
        CHECK(!dag.find_cycle(cycle) && cycle.empty());

        std::vector<std::string> order;
        CHECK(dag.topological_sort(order));
        CHECK(order.size() == dag.num_nodes());
        std::map<std::string, std::size_t> pos;
        for (std::size_t i = 0; i < order.size(); ++i) {
            pos[order[i]] = i;
        }
        CHECK(pos["query_parse"] < pos["user_profile"]);
        CHECK(pos["query_parse"] < pos["recall_inv"]);
        CHECK(pos["query_parse"] < pos["recall_graph"]);
        CHECK(pos["user_profile"] < pos["merge"]);
        CHECK(pos["recall_inv"] < pos["merge"]);
        CHECK(pos["recall_graph"] < pos["merge"]);
        CHECK(pos["merge"] < pos["rank"]);
        CHECK(pos["rank"] < pos["rerank"]);

        std::vector<std::vector<std::string> > layers;
        CHECK(dag.topological_layers(layers));
        CHECK(layers.size() == 5);
        CHECK(layers[0] == std::vector<std::string>({"query_parse", "isolated_metrics"}));
        CHECK(layers[1] == std::vector<std::string>({"user_profile", "recall_inv", "recall_graph"}));
        CHECK(layers[2] == std::vector<std::string>({"merge"}));
        CHECK(layers[3] == std::vector<std::string>({"rank"}));
        CHECK(layers[4] == std::vector<std::string>({"rerank"}));

        // Remove edge & re-check
        CHECK(dag.remove_edge("rank", "rerank"));
        CHECK(!dag.remove_edge("rank", "rerank"));
        CHECK(!dag.has_edge("rank", "rerank"));
        CHECK(dag.num_edges() == 7);
    }

    // Cycle detection: self-loop, 3-cycle with tails, deep chain & deep cycle
    {
        Dag<std::string> self_dag;
        self_dag.add_edge("A", "A");
        CHECK(self_dag.has_cycle());
        std::vector<std::string> cycle;
        CHECK(self_dag.find_cycle(cycle));
        CHECK(cycle == std::vector<std::string>({"A", "A"}));
        std::vector<std::string> order;
        CHECK(!self_dag.topological_sort(order) && order.empty());
        CHECK_THROWS(self_dag.execute_parallel(2, [](const std::string&) { CHECK(false); }),
                     std::logic_error);
    }

    {
        Dag<std::string> cyc;
        cyc.add_edge("prefix", "A");
        cyc.add_edge("A", "B");
        cyc.add_edge("B", "C");
        cyc.add_edge("C", "A");
        cyc.add_edge("C", "suffix");
        CHECK(cyc.has_cycle());

        std::vector<std::string> cycle;
        CHECK(cyc.find_cycle(cycle));
        CHECK(cycle.size() == 4);
        CHECK(cycle.front() == cycle.back());
        for (std::size_t i = 0; i + 1 < cycle.size(); ++i) {
            CHECK(cyc.has_edge(cycle[i], cycle[i + 1]));
        }
    }

    // Deep chain (50,000 nodes) acyclic + closing back-edge cycle (iterative safety)
    {
        const int kDeep = 50000;
        Dag<int> deep;
        deep.reserve(static_cast<std::size_t>(kDeep));
        for (int i = 0; i + 1 < kDeep; ++i) {
            deep.add_edge(i, i + 1);
        }
        CHECK(!deep.has_cycle());
        std::vector<int> order;
        CHECK(deep.topological_sort(order));
        CHECK(order.size() == static_cast<std::size_t>(kDeep));
        CHECK(order.front() == 0 && order.back() == kDeep - 1);

        deep.add_edge(kDeep - 1, 0);
        CHECK(deep.has_cycle());
        std::vector<int> cycle;
        CHECK(deep.find_cycle(cycle));
        CHECK(cycle.size() == static_cast<std::size_t>(kDeep) + 1);
        CHECK(cycle.front() == 0 && cycle.back() == 0);
    }

    // execute_parallel: happens-before data propagation (non-atomic array slots, TSan-verified!)
    {
        const int kNodes = 64;
        Dag<int> dag;
        for (int i = 0; i < kNodes; ++i) {
            dag.add_node(i);
            if (i > 0) {
                dag.add_edge(i / 2, i);
            }
            if (i >= 3) {
                dag.add_edge(i - 3, i);
            }
        }
        std::vector<std::uint64_t> values(static_cast<std::size_t>(kNodes), 0);
        std::vector<int> run_count(static_cast<std::size_t>(kNodes), 0);

        dag.execute_parallel(4, [&](const int& id) {
            std::uint64_t sum = 1;
            const std::vector<int> prereqs = dag.prerequisites(id);
            for (std::size_t i = 0; i < prereqs.size(); ++i) {
                sum += values[static_cast<std::size_t>(prereqs[i])];
            }
            values[static_cast<std::size_t>(id)] = sum;
            ++run_count[static_cast<std::size_t>(id)];
        });

        for (int i = 0; i < kNodes; ++i) {
            CHECK(run_count[static_cast<std::size_t>(i)] == 1);
            std::uint64_t expected = 1;
            const std::vector<int> prereqs = dag.prerequisites(i);
            for (std::size_t j = 0; j < prereqs.size(); ++j) {
                expected += values[static_cast<std::size_t>(prereqs[j])];
            }
            CHECK(values[static_cast<std::size_t>(i)] == expected);
        }
    }

    // execute_parallel: cross-layer antichain concurrency exceeds Kahn layer width
    // Layer 0: {A0, B0} (width 2), Layer 1: {B1, B2} (width 2)
    // Dynamic concurrency peak can reach 3 (A0 + B1 + B2 simultaneously)!
    {
        Dag<std::string> dag;
        dag.add_node("A0");
        dag.add_edge("B0", "B1");
        dag.add_edge("B0", "B2");

        std::mutex bmu;
        std::condition_variable bcv;
        int concurrent_three = 0;
        bool reached_three = false;

        dag.execute_parallel(3, [&](const std::string& name) {
            if (name == "B0") {
                return; // Immediately unlocks B1 and B2!
            }
            std::unique_lock<std::mutex> lk(bmu);
            ++concurrent_three;
            if (concurrent_three == 3) {
                reached_three = true;
                bcv.notify_all();
            } else {
                bcv.wait_for(lk, std::chrono::milliseconds(500),
                             [&]() { return reached_three; });
            }
        });
        CHECK(reached_three);
    }

    // execute_parallel: exception cancellation & first-error propagation
    {
        Dag<int> dag;
        for (int i = 0; i < 4; ++i) {
            dag.add_edge(i, i + 4);
        }
        std::atomic<int> downstream_ran(0);
        bool caught = false;
        try {
            dag.execute_parallel(4, [&](const int& id) {
                if (id == 0) {
                    throw std::runtime_error("boom-dag");
                }
                if (id >= 4) {
                    downstream_ran.fetch_add(1, std::memory_order_relaxed);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            });
        } catch (const std::runtime_error& e) {
            caught = (std::string(e.what()) == "boom-dag");
        }
        CHECK(caught);
        // Node 4 strictly depends on node 0, so it must NEVER have started after 0 threw.
        CHECK(downstream_ran.load() < 4);
    }

    // Exact per-node cancellation verification: node 4 strictly depends on node 0
    {
        Dag<int> dag;
        for (int i = 0; i < 4; ++i) {
            dag.add_edge(i, i + 4);
        }
        std::vector<std::atomic<int> > counts(8);
        for (std::size_t i = 0; i < counts.size(); ++i) {
            counts[i].store(0, std::memory_order_relaxed);
        }
        bool caught = false;
        try {
            dag.execute_parallel(4, [&](const int& id) {
                counts[static_cast<std::size_t>(id)].fetch_add(1, std::memory_order_relaxed);
                if (id == 0) {
                    throw std::runtime_error("exact-cancel");
                }
            });
        } catch (const std::runtime_error& e) {
            caught = (std::string(e.what()) == "exact-cancel");
        }
        CHECK(caught);
        CHECK(counts[4].load() == 0);
    }

    // execute_parallel_with_executor: real TaskExecutor (4 threads)
    {
        TaskExecutor pool(4, 64);
        const int kNodes = 48;
        Dag<int> dag;
        for (int i = 0; i < kNodes; ++i) {
            dag.add_node(i);
            if (i >= 4) {
                dag.add_edge(i - 4, i);
                dag.add_edge(i - 3, i);
            }
        }
        std::vector<std::uint64_t> values(static_cast<std::size_t>(kNodes), 0);
        dag.execute_parallel_with_executor(
            4,
            [&pool](std::function<void()> task) { return pool.submit(std::move(task)); },
            [&](const int& id) {
                std::uint64_t sum = 1;
                const std::vector<int> prereqs = dag.prerequisites(id);
                for (std::size_t j = 0; j < prereqs.size(); ++j) {
                    sum += values[static_cast<std::size_t>(prereqs[j])];
                }
                values[static_cast<std::size_t>(id)] = sum;
            });
        for (int i = 0; i < kNodes; ++i) {
            std::uint64_t expected = 1;
            const std::vector<int> prereqs = dag.prerequisites(i);
            for (std::size_t j = 0; j < prereqs.size(); ++j) {
                expected += values[static_cast<std::size_t>(prereqs[j])];
            }
            CHECK(values[static_cast<std::size_t>(i)] == expected);
        }
        pool.shutdown();
    }

    // execute_parallel_with_executor: rejecting/throwing executor, saturated 1-thread
    // recursive TaskExecutor, and deferred tasks held past return (both ok & exception)
    {
        Dag<int> dag;
        for (int i = 0; i < 8; ++i) {
            dag.add_edge(i, i + 8);
        }

        // Rejecting executor
        std::atomic<int> count(0);
        dag.execute_parallel_with_executor(
            4,
            [](std::function<void()>) -> bool { return false; },
            [&](const int&) { count.fetch_add(1, std::memory_order_relaxed); });
        CHECK(count.load() == 16);

        // Throwing executor
        count.store(0);
        dag.execute_parallel_with_executor(
            4,
            [](std::function<void()>) -> bool { throw std::runtime_error("pool full"); },
            [&](const int&) { count.fetch_add(1, std::memory_order_relaxed); });
        CHECK(count.load() == 16);

        // Saturated / recursive 1-thread TaskExecutor MUST NOT deadlock!
        // Synchronize to verify outer task runs and submits nested helpers BEFORE shutdown().
        TaskExecutor single_pool(1, 16);
        std::atomic<int> nested_ran(0);
        std::mutex smu;
        std::condition_variable scv;
        bool outer_done = false;
        CHECK(single_pool.submit([&]() {
            dag.execute_parallel_with_executor(
                4,
                [&single_pool](std::function<void()> task) {
                    return single_pool.submit(std::move(task));
                },
                [&](const int&) { nested_ran.fetch_add(1, std::memory_order_relaxed); });
            {
                std::lock_guard<std::mutex> lk(smu);
                outer_done = true;
            }
            scv.notify_all();
        }));
        {
            std::unique_lock<std::mutex> lk(smu);
            CHECK(scv.wait_for(lk, std::chrono::seconds(5), [&]() { return outer_done; }));
        }
        single_pool.shutdown();
        CHECK(nested_ran.load() == 16);

        // Deferred tasks executed AFTER execute_parallel_with_executor returns must
        // observe closed == true and NOT touch destroyed stack captures!
        std::vector<std::function<void()> > deferred;
        bool threw = false;
        {
            int stack_sentinel = 12345;
            try {
                dag.execute_parallel_with_executor(
                    4,
                    [&deferred](std::function<void()> task) -> bool {
                        deferred.push_back(std::move(task));
                        return true;
                    },
                    [&stack_sentinel](const int& id) {
                        CHECK(stack_sentinel == 12345);
                        if (id == 3) {
                            throw std::runtime_error("deferred-err");
                        }
                    });
            } catch (const std::runtime_error& e) {
                threw = (std::string(e.what()) == "deferred-err");
            }
        }
        CHECK(threw);
        CHECK(!deferred.empty());
        std::thread late([&deferred]() {
            for (std::size_t i = 0; i < deferred.size(); ++i) {
                deferred[i]();
            }
            deferred.clear();
        });
        late.join();
    }

    // Strong exception safety via AllocFaultScope on Dag::add_node & add_edge
    {
        Dag<std::string> d;
        d.add_edge("root", "child");
        for (std::size_t fail_at = 0; fail_at < 12; ++fail_at) {
            const std::string a = "new_a_" + std::to_string(fail_at);
            const std::string b = "new_b_" + std::to_string(fail_at);
            const std::size_t prev_nodes = d.num_nodes();
            const std::size_t prev_edges = d.num_edges();
            bool threw = false;
            try {
                AllocFaultScope scope(fail_at);
                d.add_edge(a, b);
            } catch (const std::bad_alloc&) {
                threw = true;
            }
            if (threw) {
                CHECK(d.num_nodes() == prev_nodes);
                CHECK(d.num_edges() == prev_edges);
                CHECK(!d.contains(a));
                CHECK(!d.contains(b));
                CHECK(!d.has_edge(a, b));
            } else {
                CHECK(d.num_nodes() == prev_nodes + 2);
                CHECK(d.num_edges() == prev_edges + 1);
                CHECK(d.has_edge(a, b));
            }
        }

        // Self-loop edge rollback on new node
        for (std::size_t fail_at = 0; fail_at < 8; ++fail_at) {
            const std::string s = "self_" + std::to_string(fail_at);
            const std::size_t prev_nodes = d.num_nodes();
            const std::size_t prev_edges = d.num_edges();
            bool threw = false;
            try {
                AllocFaultScope scope(fail_at);
                d.add_edge(s, s);
            } catch (const std::bad_alloc&) {
                threw = true;
            }
            if (threw) {
                CHECK(d.num_nodes() == prev_nodes);
                CHECK(d.num_edges() == prev_edges);
                CHECK(!d.contains(s));
            } else {
                CHECK(d.num_nodes() == prev_nodes + 1);
                CHECK(d.num_edges() == prev_edges + 1);
                CHECK(d.has_edge(s, s));
            }
        }
    }

    // Move semantics on Dag
    {
        Dag<std::string> d;
        d.add_edge("x", "y");
        Dag<std::string> moved(std::move(d));
        CHECK(d.empty() && d.num_nodes() == 0 && d.num_edges() == 0);
        CHECK(!d.contains("x"));
        CHECK(moved.num_nodes() == 2 && moved.has_edge("x", "y"));
        d.add_edge("p", "q");
        CHECK(d.num_nodes() == 2 && d.has_edge("p", "q"));
    }
}

// -----------------------------------------------------------------------------
// 6. Adversarial review regressions (Codex & Claude findings)
// -----------------------------------------------------------------------------
struct OverloadedAddressCallable {
    std::atomic<int>* count;
    void operator&() const {} // Poisoned operator& must not break std::addressof!
    void operator()(const int&) const {
        count->fetch_add(1, std::memory_order_relaxed);
    }
};

void test_review_regressions() {
    // 1. Early-exit Dijkstra must NOT expose unsettled tentative distances
    {
        // s(0)->target(1): 1, s(0)->a(2): 100, s(0)->b(3): 2, b(3)->a(2): 1
        WeightedCsrGraph<int>::Builder b(4);
        b.add_edge(0, 1, 1);
        b.add_edge(0, 2, 100);
        b.add_edge(0, 3, 2);
        b.add_edge(3, 2, 1);
        const WeightedCsrGraph<int> g = b.build();

        DijkstraOptions<int> opts;
        opts.stop_at_target = 1;
        const ShortestPathResult<int> res = g.dijkstra(0, opts);
        CHECK(!res.complete);
        CHECK(res.is_reachable(0) && res.distance_to(0) == 0);
        CHECK(res.is_reachable(1) && res.distance_to(1) == 1);
        // Vertex 2 was discovered with tentative 100 (true shortest is 3) and unsettled:
        CHECK(!res.is_reachable(2));
        CHECK_THROWS(res.distance_to(2), std::out_of_range);
        std::vector<VertexId> path;
        CHECK(!res.reconstruct_path(2, path));
    }

    // 2. Floating-point cutoff exact equality: 0.9 + 0.1 == 1.0 with max_distance == 1.0
    {
        WeightedCsrGraph<double>::Builder b(3);
        b.add_edge(0, 1, 0.9);
        b.add_edge(1, 2, 0.1);
        const WeightedCsrGraph<double> g = b.build();

        std::vector<VertexId> path;
        double dist = 0.0;
        CHECK(g.shortest_path(0, 2, 1.0, path, &dist));
        CHECK(path == std::vector<VertexId>({0, 1, 2}));
        CHECK(dist <= 1.0);

        CHECK(g.astar(0, 2, 1.0, [](VertexId v) { return (v == 1) ? 0.1 : 0.0; }, path, &dist));
        CHECK(path == std::vector<VertexId>({0, 1, 2}));
        CHECK(dist <= 1.0);
    }

    // 3. No spurious overflow_error when a representable shorter path exists
    {
        const std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
        WeightedCsrGraph<std::uint64_t>::Builder b(5);
        b.add_edge(0, 1, 10);
        b.add_edge(1, 2, kMax); // 10 + UINT64_MAX overflows
        b.add_edge(0, 3, 20);
        b.add_edge(3, 2, 5);    // representable path 0->3->2 has cost 25!
        b.add_edge(1, 4, kMax); // unrelated overflowing branch to 4 (no alternate)
        const WeightedCsrGraph<std::uint64_t> g = b.build();

        // Point-to-point shortest_path(0, 2) and astar(0, 2) must succeed cleanly!
        std::vector<VertexId> path;
        std::uint64_t dist = 0;
        CHECK(g.shortest_path(0, 2, path, &dist));
        CHECK(dist == 25 && path == std::vector<VertexId>({0, 3, 2}));

        CHECK(g.astar(0, 2, [](VertexId) { return std::uint64_t(0); }, path, &dist));
        CHECK(dist == 25 && path == std::vector<VertexId>({0, 3, 2}));

        // Full graph without vertex 4: vertex 2 has shorter path 25, so full Dijkstra & BF succeed!
        WeightedCsrGraph<std::uint64_t>::Builder b2(4);
        b2.add_edge(0, 1, 10);
        b2.add_edge(1, 2, kMax);
        b2.add_edge(0, 3, 20);
        b2.add_edge(3, 2, 5);
        const WeightedCsrGraph<std::uint64_t> g2 = b2.build();
        const ShortestPathResult<std::uint64_t> d2 = g2.dijkstra(0);
        CHECK(d2.distance_to(2) == 25);

        ShortestPathResult<std::uint64_t> bf2;
        CHECK(g2.bellman_ford(0, bf2));
        CHECK(bf2.distance_to(2) == 25);

        // Unrelated disconnected target 5 with overflowing edge 0->1->4 returns unreachable (false), not overflow!
        WeightedCsrGraph<std::uint64_t>::Builder b3(6);
        b3.add_edge(0, 1, 10);
        b3.add_edge(1, 4, kMax);
        const WeightedCsrGraph<std::uint64_t> g3 = b3.build();
        CHECK(!g3.shortest_path(0, 5, path, &dist));
        CHECK(!g3.astar(0, 5, [](VertexId) { return std::uint64_t(0); }, path, &dist));
        // Target 4 IS only reachable via overflowing path -> throws overflow_error!
        CHECK_THROWS(g3.shortest_path(0, 4, path, &dist), std::overflow_error);
        CHECK_THROWS(g3.astar(0, 4, [](VertexId) { return std::uint64_t(0); }, path, &dist),
                     std::overflow_error);
    }

    // 4. Bellman-Ford negative cycle with large negative int weights returns false (not overflow_error)
    {
        const int kMin = std::numeric_limits<int>::min();
        WeightedCsrGraph<int>::Builder b(3);
        b.add_edge(0, 1, kMin + 10);
        b.add_edge(1, 2, -20);
        b.add_edge(2, 1, -20);
        const WeightedCsrGraph<int> g = b.build();
        ShortestPathResult<int> bf;
        CHECK(!g.bellman_ford(0, bf));
        CHECK(bf.reached.empty());
    }

    // 5. Kruskal MSF representable final sum with overflowing sorted prefix (INT_MIN, -1, INT_MAX -> -2)
    {
        const int kMin = std::numeric_limits<int>::min();
        const int kMax = std::numeric_limits<int>::max();
        WeightedCsrGraph<int>::Builder b(4);
        b.add_undirected_edge(0, 1, kMin);
        b.add_undirected_edge(1, 2, -1);
        b.add_undirected_edge(2, 3, kMax);
        const WeightedCsrGraph<int> g = b.build();
        const SpanningForestResult<int> msf = g.minimum_spanning_forest();
        CHECK(msf.num_components == 1);
        CHECK(msf.edges.size() == 3);
        CHECK(msf.total_weight == -2);
    }

    // 6. WeightedCsrGraph::shortest_path rejects kInvalidVertex target consistently
    {
        WeightedCsrGraph<int>::Builder b(2);
        b.add_edge(0, 1, 1);
        const WeightedCsrGraph<int> g = b.build();
        std::vector<VertexId> path;
        CHECK_THROWS(g.shortest_path(0, kInvalidVertex, path), std::out_of_range);
        CHECK_THROWS(g.shortest_path(0, kInvalidVertex, 10, path), std::out_of_range);
    }

    // 7. A* validates floating-point heuristic BEFORE integer conversion (no UB on NaN/Inf/huge)
    {
        WeightedCsrGraph<int>::Builder b(2);
        b.add_edge(0, 1, 5);
        const WeightedCsrGraph<int> g = b.build();
        std::vector<VertexId> path;
        CHECK_THROWS(g.astar(0, 1, [](VertexId) {
            return std::numeric_limits<double>::quiet_NaN();
        }, path), std::invalid_argument);
        CHECK_THROWS(g.astar(0, 1, [](VertexId) {
            return -0.5;
        }, path), std::invalid_argument);
        CHECK_THROWS(g.astar(0, 1, [](VertexId) {
            return 1e30;
        }, path), std::overflow_error);
        // With max_distance, huge heuristic prunes cleanly
        CHECK(!g.astar(0, 1, 10, [](VertexId) { return 1e30; }, path));
    }

    // 8. Dag::add_edge aliased key argument across vector reallocation
    {
        Dag<std::string> d;
        d.add_node("node_0");
        for (int i = 1; i <= 64; ++i) {
            const std::string& aliased_existing = d.nodes()[0];
            const std::string fresh = "fresh_" + std::to_string(i);
            CHECK(d.add_edge(fresh, aliased_existing));
            CHECK(d.has_edge(fresh, "node_0"));
        }
        CHECK(d.num_nodes() == 65);
        CHECK(d.num_edges() == 64);
    }

    // 9. Dag::add_edge strong exception safety across forced unordered_map rehash
    //    with fresh identical baseline per allocation failure index
    {
        for (std::size_t fail_at = 0; fail_at < 20; ++fail_at) {
            Dag<std::string> d;
            // Fill right up to bucket boundary so adding two nodes forces a rehash
            for (int i = 0; i < 11; ++i) {
                d.add_node("k_" + std::to_string(i));
            }
            d.add_edge("k_0", "k_1");
            const std::size_t base_n = d.num_nodes();
            const std::size_t base_e = d.num_edges();

            bool threw = false;
            try {
                AllocFaultScope scope(fail_at);
                d.add_edge("trigger_rehash_a", "trigger_rehash_b");
            } catch (const std::bad_alloc&) {
                threw = true;
            }
            if (threw) {
                CHECK(d.num_nodes() == base_n);
                CHECK(d.num_edges() == base_e);
                CHECK(!d.contains("trigger_rehash_a"));
                CHECK(!d.contains("trigger_rehash_b"));
                CHECK(d.has_edge("k_0", "k_1"));
            } else {
                CHECK(d.num_nodes() == base_n + 2);
                CHECK(d.num_edges() == base_e + 1);
                CHECK(d.has_edge("trigger_rehash_a", "trigger_rehash_b"));
            }
        }
    }

    // 10. Copy-assignment strong exception safety on DisjointSet, CsrGraph, WeightedCsrGraph, Dag
    {
        DisjointSet src_dsu(50);
        src_dsu.unite(0, 1);
        for (std::size_t fail_at = 0; fail_at < 5; ++fail_at) {
            DisjointSet dst(3);
            dst.unite(1, 2);
            bool threw = false;
            try {
                AllocFaultScope scope(fail_at);
                dst = src_dsu;
            } catch (const std::bad_alloc&) {
                threw = true;
            }
            if (threw) {
                CHECK(dst.num_vertices() == 3);
                CHECK(dst.num_components() == 2);
                CHECK(dst.connected(1, 2));
                CHECK(!dst.connected(0, 1));
            } else {
                CHECK(dst.num_vertices() == 50 && dst.connected(0, 1));
            }
        }

        WeightedCsrGraph<double>::Builder wb(32);
        for (VertexId i = 0; i + 1 < 32; ++i) {
            wb.add_edge(i, i + 1, 1.5);
        }
        const WeightedCsrGraph<double> src_wg = wb.build();
        for (std::size_t fail_at = 0; fail_at < 6; ++fail_at) {
            WeightedCsrGraph<double>::Builder db(2);
            db.add_edge(0, 1, 9.0);
            WeightedCsrGraph<double> dst = db.build();
            bool threw = false;
            try {
                AllocFaultScope scope(fail_at);
                dst = src_wg;
            } catch (const std::bad_alloc&) {
                threw = true;
            }
            if (threw) {
                CHECK(dst.num_vertices() == 2 && dst.num_edges() == 1);
                CHECK(std::fabs(dst.weights(0)[0] - 9.0) < 1e-12);
            } else {
                CHECK(dst.num_vertices() == 32 && dst.num_edges() == 31);
            }
        }

        Dag<std::string> src_dag;
        for (int i = 0; i < 20; ++i) {
            src_dag.add_edge("s_" + std::to_string(i), "s_" + std::to_string(i + 1));
        }
        for (std::size_t fail_at = 0; fail_at < 10; ++fail_at) {
            Dag<std::string> dst;
            dst.add_edge("orig_a", "orig_b");
            bool threw = false;
            try {
                AllocFaultScope scope(fail_at);
                dst = src_dag;
            } catch (const std::bad_alloc&) {
                threw = true;
            }
            if (threw) {
                CHECK(dst.num_nodes() == 2 && dst.num_edges() == 1);
                CHECK(dst.has_edge("orig_a", "orig_b"));
            } else {
                CHECK(dst.num_nodes() == 21 && dst.num_edges() == 20);
            }
        }
    }

    // 11. execute_parallel_with_executor: pool worker fans out unlocked siblings even
    //     while caller is blocked on a slow node (no parallelism collapse!)
    {
        TaskExecutor pool(3, 32);
        Dag<std::string> dag;
        dag.add_node("A0");
        dag.add_edge("B0", "B1");
        dag.add_edge("B0", "B2");

        std::mutex bmu;
        std::condition_variable bcv;
        int at_barrier = 0;
        bool all_three = false;

        dag.execute_parallel_with_executor(
            3,
            [&pool](std::function<void()> task) { return pool.submit(std::move(task)); },
            [&](const std::string& name) {
                if (name == "B0") {
                    return; // Unlocks B1 and B2 while another thread sits in A0
                }
                std::unique_lock<std::mutex> lk(bmu);
                ++at_barrier;
                if (at_barrier == 3) {
                    all_three = true;
                    bcv.notify_all();
                } else {
                    bcv.wait_for(lk, std::chrono::seconds(2),
                                 [&]() { return all_three; });
                }
            });
        pool.shutdown();
        CHECK(all_three);
    }

    // 12. Deferred tasks executed after BOTH Dag and callable are destroyed (success path),
    //     plus misbehaving inline-running-then-throwing submit_fn, plus overloaded operator&
    {
        std::vector<std::function<void()> > deferred;
        {
            Dag<int> local_dag;
            for (int i = 0; i < 6; ++i) {
                local_dag.add_edge(i, i + 6);
            }
            std::atomic<int> ran(0);
            OverloadedAddressCallable callable{&ran};
            local_dag.execute_parallel_with_executor(
                4,
                [&deferred](std::function<void()> task) -> bool {
                    deferred.push_back(std::move(task));
                    return true;
                },
                callable);
            CHECK(ran.load() == 12);
        } // Both local_dag and callable are now destroyed!
        CHECK(!deferred.empty());
        std::thread late([&deferred]() {
            for (std::size_t i = 0; i < deferred.size(); ++i) {
                deferred[i]();
            }
            deferred.clear();
        });
        late.join();

        // Misbehaving submit_fn that executes the task inline and STILL throws!
        Dag<int> dag;
        for (int i = 0; i < 4; ++i) {
            dag.add_edge(i, i + 4);
        }
        std::atomic<int> ran2(0);
        dag.execute_parallel_with_executor(
            4,
            [](std::function<void()> task) -> bool {
                task();
                throw std::runtime_error("throw after inline run");
            },
            [&ran2](const int&) { ran2.fetch_add(1, std::memory_order_relaxed); });
        CHECK(ran2.load() == 8);
    }
}

} // namespace

int main() {
    test_disjoint_set();
    test_csr_graph();
    test_weighted_csr_graph_and_dijkstra();
    test_double_buffer_graph_hot_reload();
    test_dag();
    test_review_regressions();
    std::cout << "[PASS] test_graph (" << g_checks.load()
              << " checks passed)" << std::endl;
    return 0;
}
