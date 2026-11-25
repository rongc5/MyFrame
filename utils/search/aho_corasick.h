#pragma once

#include <cstddef>
#include <cstdint>
#include <queue>
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
//   - Strict C++11, header-only, thread-safe for concurrent reads once build() completes.
//   - Memory-efficient sorted edge list per node (avoids 256-entry pointer arrays per
//     node on large UTF-8 dictionaries) plus an O(1) 256-entry root dispatch table.
//   - Explicit dictionary suffix link (dict_link) ensures nested and overlapping short
//     patterns inside longer patterns are always reported.
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
    std::uint32_t add_pattern(const std::string& pattern, std::uint64_t tag = 0) {
        if (pattern.empty()) {
            throw std::invalid_argument("AhoCorasick pattern must be non-empty");
        }
        const std::uint32_t pid = static_cast<std::uint32_t>(patterns_.size());
        patterns_.push_back(PatternMeta{pattern.size(), tag});

        std::uint32_t cur = 0;
        for (std::size_t i = 0; i < pattern.size(); ++i) {
            const unsigned char ch = static_cast<unsigned char>(pattern[i]);
            std::uint32_t nxt = find_edge(cur, ch);
            if (nxt == kNoNode) {
                nxt = static_cast<std::uint32_t>(nodes_.size());
                nodes_.push_back(Node());
                insert_edge(cur, ch, nxt);
            }
            cur = nxt;
        }
        nodes_[cur].outputs.push_back(pid);
        built_ = false;
        return pid;
    }

    // Builds failure links, dictionary output links, and root fast-dispatch table.
    // Safe to call again if more patterns were added.
    void build() {
        reset_root_table();
        std::queue<std::uint32_t> q;

        for (std::size_t i = 0; i < nodes_[0].edges.size(); ++i) {
            const Edge& e = nodes_[0].edges[i];
            root_next_[e.ch] = e.next;
            nodes_[e.next].fail = 0;
            nodes_[e.next].dict_link = 0;
            q.push(e.next);
        }

        while (!q.empty()) {
            const std::uint32_t u = q.front();
            q.pop();

            for (std::size_t i = 0; i < nodes_[u].edges.size(); ++i) {
                const Edge e = nodes_[u].edges[i];
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

                q.push(v);
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
        nodes_.assign(1, Node());
        patterns_.clear();
        reset_root_table();
        built_ = false;
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

    void insert_edge(std::uint32_t node_idx, unsigned char ch, std::uint32_t next_idx) {
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
