#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace myframe {
namespace search {

// Compact byte-level Aho-Corasick multi-pattern string matcher (UTF-8 safe).
//
// Common ads/search uses:
//   - Prohibited / sensitive keyword filtering in ad review & UGC moderation
//   - Query entity / brand / trigger-word extraction in a single O(N + matches) pass
//   - Multi-keyword stopword and rule tagging
//
// Properties:
//   - Strict C++11/C++17, header-only, thread-safe for concurrent reads once build() completes.
//   - Memory-efficient sorted edge list per node (avoids 256-entry pointer arrays per
//     node on large UTF-8 dictionaries) plus an O(1) 256-entry root dispatch table.
//   - Explicit dictionary suffix link (dict_link) ensures nested and overlapping short
//     patterns inside longer patterns are always reported.
//   - Failure-safe mutation/build: `built_` is invalidated before modifying state and
//     failed add_pattern() rolls back partial trie/metadata changes, so a failed
//     mutation or build never advertises a scan-ready automaton and recovers cleanly
//     on a subsequent build().
class AhoCorasick {
public:
    struct Match {
        std::uint32_t pattern_id;
        std::uint64_t tag;
        std::size_t start;
        std::size_t length;
    };

    AhoCorasick() : nodes_(1), patterns_(), built_(false) {
        reset_root_table();
    }

    // Adds a non-empty pattern and returns its assigned 0-based pattern_id.
    // Must call build() after adding patterns before scanning.
    // If an allocation fails, `built_` is invalidated, partial trie/pattern changes
    // are rolled back, and a subsequent build() recovers the prior pattern set.
    std::uint32_t add_pattern(const std::string& pattern, std::uint64_t tag = 0) {
        if (pattern.empty()) {
            throw std::invalid_argument("AhoCorasick pattern must be non-empty");
        }

        // Walk existing prefix read-only before mutating any state.
        std::uint32_t cur = 0;
        std::size_t pos = 0;
        while (pos < pattern.size()) {
            const unsigned char ch = static_cast<unsigned char>(pattern[pos]);
            const std::uint32_t nxt = find_edge(cur, ch);
            if (nxt == kNoNode) {
                break;
            }
            cur = nxt;
            ++pos;
        }

        const std::size_t new_nodes_needed = pattern.size() - pos;
        const std::size_t max_items = static_cast<std::size_t>(kNoNode);
        if (patterns_.size() >= max_items ||
            nodes_.size() > max_items ||
            new_nodes_needed > max_items - nodes_.size() ||
            patterns_.max_size() - patterns_.size() < 1 ||
            new_nodes_needed > nodes_.max_size() - nodes_.size() ||
            (new_nodes_needed == 0 &&
             nodes_[cur].outputs.max_size() - nodes_[cur].outputs.size() < 1)) {
            throw std::length_error("AhoCorasick capacity exceeded");
        }

        // Invalidate built state BEFORE any allocation or structural mutation so a
        // failed mutation can never leave `built_ == true`.
        built_ = false;

        const std::uint32_t pid = static_cast<std::uint32_t>(patterns_.size());
        const std::size_t orig_nodes_size = nodes_.size();
        const std::size_t orig_patterns_size = patterns_.size();
        const std::uint32_t attach_node = cur;
        std::size_t attach_edge_idx = 0;
        bool attach_edge_inserted = false;

        try {
            if (new_nodes_needed == 0) {
                // Pattern terminates on an existing node: reserve both outputs and
                // patterns_ geometrically first so commit cannot throw while preserving
                // amortized O(1) growth.
                reserve_geometric(nodes_[cur].outputs, 1, max_items);
                reserve_geometric(patterns_, 1, max_items);
                nodes_[cur].outputs.push_back(pid);
                patterns_.push_back(PatternMeta{pattern.size(), tag});
                return pid;
            }

            reserve_geometric(patterns_, 1, max_items);
            reserve_geometric(nodes_, new_nodes_needed, max_items);

            const std::uint32_t first_new = static_cast<std::uint32_t>(orig_nodes_size);
            const unsigned char first_ch = static_cast<unsigned char>(pattern[pos]);

            for (std::size_t k = 0; k < new_nodes_needed; ++k) {
                nodes_.push_back(Node());
            }
            for (std::size_t k = 0; k + 1 < new_nodes_needed; ++k) {
                const std::uint32_t u = first_new + static_cast<std::uint32_t>(k);
                const std::uint32_t v = u + 1;
                const unsigned char ch = static_cast<unsigned char>(pattern[pos + 1 + k]);
                nodes_[u].edges.push_back(Edge{ch, v});
            }
            const std::uint32_t last_new =
                first_new + static_cast<std::uint32_t>(new_nodes_needed - 1);
            nodes_[last_new].outputs.push_back(pid);

            attach_edge_idx = insert_edge(attach_node, first_ch, first_new);
            attach_edge_inserted = true;

            patterns_.push_back(PatternMeta{pattern.size(), tag});
            return pid;
        } catch (...) {
            if (attach_edge_inserted) {
                std::vector<Edge>& edges = nodes_[attach_node].edges;
                edges.erase(edges.begin() + static_cast<std::ptrdiff_t>(attach_edge_idx));
            }
            while (nodes_.size() > orig_nodes_size) {
                nodes_.pop_back();
            }
            while (patterns_.size() > orig_patterns_size) {
                patterns_.pop_back();
            }
            throw;
        }
    }

    // Builds failure links, dictionary output links, and root fast-dispatch table.
    // Safe to call again if more patterns were added or to recover after a failed build().
    void build() {
        built_ = false;

        // Pre-allocate the BFS queue before mutating root_next_ or failure links:
        // every non-root trie node is enqueued at most once, so nodes_.size() - 1
        // is an exact upper bound and the BFS loop performs zero allocations.
        std::vector<std::uint32_t> bfs;
        if (nodes_.size() > 1) {
            bfs.reserve(nodes_.size() - 1);
        }

        reset_root_table();
        if (nodes_.empty()) {
            return;
        }
        nodes_[0].fail = 0;
        nodes_[0].dict_link = 0;

        for (std::size_t i = 0; i < nodes_[0].edges.size(); ++i) {
            const Edge& e = nodes_[0].edges[i];
            root_next_[e.ch] = e.next;
            nodes_[e.next].fail = 0;
            nodes_[e.next].dict_link = 0;
            bfs.push_back(e.next);
        }

        for (std::size_t head = 0; head < bfs.size(); ++head) {
            const std::uint32_t u = bfs[head];
            const std::vector<Edge>& edges = nodes_[u].edges;

            for (std::size_t i = 0; i < edges.size(); ++i) {
                const Edge e = edges[i];
                const std::uint32_t v = e.next;

                std::uint32_t f = nodes_[u].fail;
                while (f != 0 && find_edge(f, e.ch) == kNoNode) {
                    f = nodes_[f].fail;
                }
                const std::uint32_t cand = (f == 0) ? root_next_[e.ch] : find_edge(f, e.ch);
                nodes_[v].fail = (cand != kNoNode) ? cand : 0;

                const std::uint32_t vf = nodes_[v].fail;
                nodes_[v].dict_link =
                    !nodes_[vf].outputs.empty() ? vf : nodes_[vf].dict_link;

                bfs.push_back(v);
            }
        }

        built_ = true;
    }

    // Scans `[data, data + len)` and invokes `fn(const Match&)`.
    // Callback returns bool: return false to stop scanning early.
    template <typename Fn>
    void scan_until(const char* data, std::size_t len, Fn&& fn) const {
        ensure_built();
        if (data == nullptr || len == 0 || patterns_.empty()) {
            return;
        }

        std::uint32_t state = 0;
        for (std::size_t i = 0; i < len; ++i) {
            const unsigned char ch = static_cast<unsigned char>(data[i]);
            state = step(state, ch);

            std::uint32_t out_node =
                !nodes_[state].outputs.empty() ? state : nodes_[state].dict_link;
            while (out_node != 0) {
                const std::vector<std::uint32_t>& outs = nodes_[out_node].outputs;
                for (std::size_t oi = 0; oi < outs.size(); ++oi) {
                    const std::uint32_t pid = outs[oi];
                    const PatternMeta& meta = patterns_[pid];
                    const Match m = {pid, meta.tag, i + 1 - meta.length, meta.length};
                    if (!fn(m)) {
                        return;
                    }
                }
                out_node = nodes_[out_node].dict_link;
            }
        }
    }

    template <typename Fn>
    void scan_until(const std::string& text, Fn&& fn) const {
        scan_until(text.data(), text.size(), std::forward<Fn>(fn));
    }

    // Scans the entire text invoking `fn(const Match&)` (void callback).
    template <typename Fn>
    void scan(const char* data, std::size_t len, Fn&& fn) const {
        scan_until(data, len, [&fn](const Match& m) -> bool {
            fn(m);
            return true;
        });
    }

    template <typename Fn>
    void scan(const std::string& text, Fn&& fn) const {
        scan(text.data(), text.size(), std::forward<Fn>(fn));
    }

    bool contains_any(const char* data, std::size_t len) const {
        bool found = false;
        scan_until(data, len, [&found](const Match&) -> bool {
            found = true;
            return false;
        });
        return found;
    }

    bool contains_any(const std::string& text) const {
        return contains_any(text.data(), text.size());
    }

    std::vector<Match> find_all(const char* data, std::size_t len) const {
        std::vector<Match> matches;
        scan(data, len, [&matches](const Match& m) {
            matches.push_back(m);
        });
        return matches;
    }

    std::vector<Match> find_all(const std::string& text) const {
        return find_all(text.data(), text.size());
    }

    void clear() {
        built_ = false;
        patterns_.clear();
        reset_root_table();
        if (nodes_.empty()) {
            nodes_.resize(1);
        } else {
            nodes_.resize(1);
            nodes_[0].edges.clear();
            nodes_[0].outputs.clear();
            nodes_[0].fail = 0;
            nodes_[0].dict_link = 0;
        }
    }

    bool built() const noexcept { return built_; }
    std::size_t pattern_count() const noexcept { return patterns_.size(); }
    std::size_t node_count() const noexcept { return nodes_.size(); }

private:
    static const std::uint32_t kNoNode = 0xffffffffU;

    struct Edge {
        unsigned char ch;
        std::uint32_t next;
    };

    struct Node {
        std::vector<Edge> edges;
        std::vector<std::uint32_t> outputs;
        std::uint32_t fail;
        std::uint32_t dict_link;

        Node() : edges(), outputs(), fail(0), dict_link(0) {}
    };

    struct PatternMeta {
        std::size_t length;
        std::uint64_t tag;
    };

    template <typename T>
    static void reserve_geometric(std::vector<T>& vec,
                                  std::size_t add_count,
                                  std::size_t max_limit) {
        if (add_count == 0) {
            return;
        }
        const std::size_t vec_max = vec.max_size();
        const std::size_t limit = (max_limit < vec_max) ? max_limit : vec_max;
        const std::size_t cur_size = vec.size();
        if (cur_size > limit || add_count > limit - cur_size) {
            throw std::length_error("AhoCorasick capacity exceeded");
        }
        const std::size_t required = cur_size + add_count;
        const std::size_t cap = vec.capacity();
        if (required <= cap) {
            return;
        }
        std::size_t target = (cap >= limit - cap) ? limit : (cap * 2);
        if (target < required) {
            target = required;
        }
        vec.reserve(target);
    }

    void reset_root_table() noexcept {
        for (std::size_t i = 0; i < 256; ++i) {
            root_next_[i] = 0;
        }
    }

    void ensure_built() const {
        if (!built_) {
            throw std::logic_error("AhoCorasick::build() must be called before matching");
        }
    }

    std::uint32_t find_edge(std::uint32_t node_idx, unsigned char ch) const noexcept {
        const std::vector<Edge>& edges = nodes_[node_idx].edges;
        std::size_t lo = 0;
        std::size_t hi = edges.size();
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (edges[mid].ch < ch) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo < edges.size() && edges[lo].ch == ch) {
            return edges[lo].next;
        }
        return kNoNode;
    }

    std::size_t insert_edge(std::uint32_t node_idx, unsigned char ch, std::uint32_t next_idx) {
        std::vector<Edge>& edges = nodes_[node_idx].edges;
        std::size_t lo = 0;
        std::size_t hi = edges.size();
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (edges[mid].ch < ch) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        edges.insert(edges.begin() + static_cast<std::ptrdiff_t>(lo), Edge{ch, next_idx});
        return lo;
    }

    std::uint32_t step(std::uint32_t state, unsigned char ch) const noexcept {
        if (state == 0) {
            return root_next_[ch];
        }
        while (state != 0) {
            const std::uint32_t nxt = find_edge(state, ch);
            if (nxt != kNoNode) {
                return nxt;
            }
            state = nodes_[state].fail;
        }
        return root_next_[ch];
    }

    std::vector<Node> nodes_;
    std::vector<PatternMeta> patterns_;
    std::uint32_t root_next_[256];
    bool built_;
};

} // namespace search
} // namespace myframe
