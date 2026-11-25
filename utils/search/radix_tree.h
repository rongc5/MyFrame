#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace myframe {
namespace search {

template <typename V>
class SuffixMatcher;

// Compressed prefix tree (Radix Tree / Patricia Trie) for byte strings (UTF-8 safe).
//
// Common ads/search uses:
//   - Query suggestion / auto-complete via prefix_scan(prefix, fn, limit)
//   - URL / category / routing rules via longest_prefix_match(text, ...)
//   - Compact ordered dictionary with automatic edge splitting and erase coalescing
//
// Properties:
//   - Strict C++11/C++17, header-only, supports move-only and non-default-constructible V.
//   - Children sorted by unsigned first byte: compact memory (no 256-pointer blowup
//     per node) and deterministic lexicographical prefix_scan order.
//   - Zero-allocation noexcept move operations leaving moved-from trees empty and reusable.
//   - Allocation-free iterative clear() and destructor prevent stack overflow on
//     deeply chained keys without risking std::bad_alloc inside noexcept teardown.
//   - Strong exception safety on insert splits and erase coalescing: all required
//     allocations are prepared before committing structural ownership changes.
template <typename V>
class RadixTree {
public:
    typedef std::string key_type;
    typedef V mapped_type;

    RadixTree() noexcept : root_(), size_(0) {}

    ~RadixTree() noexcept {
        clear();
    }

    RadixTree(const RadixTree&) = delete;
    RadixTree& operator=(const RadixTree&) = delete;

    RadixTree(RadixTree&& other) noexcept
        : root_(), size_(other.size_) {
        root_.value = std::move(other.root_.value);
        root_.children.swap(other.root_.children);
        other.size_ = 0;
    }

    RadixTree& operator=(RadixTree&& other) noexcept {
        if (this != &other) {
            clear();
            root_.value = std::move(other.root_.value);
            root_.children.swap(other.root_.children);
            size_ = other.size_;
            other.size_ = 0;
        }
        return *this;
    }

    // Inserts or updates `key`. Returns true if inserted new key, false if overwritten.
    bool insert(const std::string& key, const V& value) {
        return insert_ptr(key, std::unique_ptr<V>(new V(value)));
    }

    bool insert(const std::string& key, V&& value) {
        return insert_ptr(key, std::unique_ptr<V>(new V(std::move(value))));
    }

    template <typename... Args>
    bool emplace(const std::string& key, Args&&... args) {
        return insert_ptr(key, std::unique_ptr<V>(new V(std::forward<Args>(args)...)));
    }

    V* find(const std::string& key) noexcept {
        Node* n = find_node(key);
        return (n != nullptr && n->value != nullptr) ? n->value.get() : nullptr;
    }

    const V* find(const std::string& key) const noexcept {
        const Node* n = find_node(key);
        return (n != nullptr && n->value != nullptr) ? n->value.get() : nullptr;
    }

    bool contains(const std::string& key) const noexcept {
        return find(key) != nullptr;
    }

    // Erases `key` and coalesces redundant internal nodes. Returns true if removed.
    // Strong exception guarantee: if preparing path or merged edge throws
    // std::bad_alloc, no keys or structural invariants are modified.
    bool erase(const std::string& key) {
        if (key.empty()) {
            if (!root_.value) {
                return false;
            }
            root_.value.reset();
            --size_;
            return true;
        }

        struct PathStep {
            Node* parent;
            std::size_t child_idx;
        };
        std::vector<PathStep> path;

        Node* cur = &root_;
        std::size_t pos = 0;
        while (pos < key.size()) {
            const unsigned char first_byte = static_cast<unsigned char>(key[pos]);
            const std::size_t idx = find_child_index(cur, first_byte);
            if (idx == cur->children.size()) {
                return false;
            }
            Node* child = cur->children[idx].get();
            const std::size_t common = common_prefix_len(
                child->edge.data(), child->edge.size(),
                key.data() + pos, key.size() - pos);
            if (common < child->edge.size()) {
                return false;
            }
            path.push_back(PathStep{cur, idx});
            pos += common;
            cur = child;
        }

        if (!cur->value) {
            return false;
        }

        // Plan pruning and single-child coalescing read-only so any required
        // merged-edge allocation completes BEFORE mutating any node or value.
        Node* coalesce_node = nullptr;
        std::size_t coalesce_step_idx = 0;
        Node* coalesce_survivor = nullptr;
        std::size_t prune_top_step = path.size();

        if (cur->children.size() == 1) {
            coalesce_node = cur;
            coalesce_step_idx = path.size() - 1;
            coalesce_survivor = cur->children[0].get();
        } else if (cur->children.empty()) {
            std::size_t idx = path.size() - 1;
            prune_top_step = idx;
            while (idx > 0) {
                Node* parent = path[idx].parent;
                if (parent->value) {
                    break;
                }
                if (parent->children.size() == 1) {
                    --idx;
                    prune_top_step = idx;
                } else if (parent->children.size() == 2) {
                    const std::size_t survivor_idx = (path[idx].child_idx == 0) ? 1 : 0;
                    coalesce_node = parent;
                    coalesce_step_idx = idx - 1;
                    coalesce_survivor = parent->children[survivor_idx].get();
                    break;
                } else {
                    break;
                }
            }
        }

        std::string merged_edge;
        if (coalesce_node != nullptr) {
            merged_edge.reserve(coalesce_node->edge.size() + coalesce_survivor->edge.size());
            merged_edge.append(coalesce_node->edge);
            merged_edge.append(coalesce_survivor->edge);
        }

        // --- Commit point (nothrow from here) ---
        cur->value.reset();
        --size_;

        if (prune_top_step < path.size()) {
            for (std::size_t i = path.size(); i > prune_top_step; --i) {
                const PathStep& step = path[i - 1];
                step.parent->children.erase(
                    step.parent->children.begin() +
                    static_cast<std::ptrdiff_t>(step.child_idx));
            }
        }

        if (coalesce_node != nullptr) {
            coalesce_survivor->edge.swap(merged_edge);
            std::unique_ptr<Node> only_child = std::move(coalesce_node->children[0]);
            const PathStep& cstep = path[coalesce_step_idx];
            cstep.parent->children[cstep.child_idx] = std::move(only_child);
        }
        return true;
    }

    // Finds the longest key in the tree that is a prefix of `text`.
    // Returns pointer to the matched value (or nullptr if none). If `matched_len`
    // is non-null, writes the byte length of the matched prefix.
    const V* longest_prefix_match(const std::string& text,
                                  std::size_t* matched_len = nullptr) const noexcept {
        const V* best_val = nullptr;
        std::size_t best_len = 0;

        const Node* cur = &root_;
        if (cur->value) {
            best_val = cur->value.get();
            best_len = 0;
        }

        std::size_t pos = 0;
        while (pos < text.size()) {
            const unsigned char first_byte = static_cast<unsigned char>(text[pos]);
            const std::size_t idx = find_child_index(cur, first_byte);
            if (idx == cur->children.size()) {
                break;
            }
            const Node* child = cur->children[idx].get();
            if (text.size() - pos < child->edge.size()) {
                break;
            }
            const std::size_t common = common_prefix_len(
                child->edge.data(), child->edge.size(),
                text.data() + pos, child->edge.size());
            if (common < child->edge.size()) {
                break;
            }
            pos += child->edge.size();
            cur = child;
            if (cur->value) {
                best_val = cur->value.get();
                best_len = pos;
            }
        }

        if (best_val != nullptr && matched_len != nullptr) {
            *matched_len = best_len;
        }
        return best_val;
    }

    // Visits up to `limit` entries whose key starts with `prefix`, in unsigned-byte
    // lexicographical order. Calls `fn(const std::string& key, const V& value)`.
    // Returns number of visited entries.
    template <typename Fn>
    std::size_t prefix_scan(
        const std::string& prefix,
        Fn&& fn,
        std::size_t limit = std::numeric_limits<std::size_t>::max()) const {
        if (limit == 0) {
            return 0;
        }

        const Node* cur = &root_;
        std::string current_key;
        std::size_t pos = 0;

        while (pos < prefix.size()) {
            const unsigned char first_byte = static_cast<unsigned char>(prefix[pos]);
            const std::size_t idx = find_child_index(cur, first_byte);
            if (idx == cur->children.size()) {
                return 0;
            }
            const Node* child = cur->children[idx].get();
            const std::size_t rem = prefix.size() - pos;
            const std::size_t common = common_prefix_len(
                child->edge.data(), child->edge.size(),
                prefix.data() + pos, rem);
            if (common < child->edge.size()) {
                if (common == rem) {
                    // `prefix` ends inside `child->edge`: the entire subtree
                    // rooted at `child` matches `prefix`.
                    current_key.append(child->edge);
                    cur = child;
                    pos = prefix.size();
                    break;
                }
                return 0;
            }
            current_key.append(child->edge);
            pos += common;
            cur = child;
        }

        std::size_t visited = 0;
        dfs_collect(cur, current_key, fn, limit, visited);
        return visited;
    }

    // Zero-allocation iterative teardown avoids both stack overflow on deep
    // trees and dynamic allocation inside noexcept clear()/destructor.
    void clear() noexcept {
        Node* head = nullptr;
        while (!root_.children.empty()) {
            Node* child = root_.children.back().release();
            root_.children.pop_back();
            if (child != nullptr) {
                child->teardown_next = head;
                head = child;
            }
        }
        std::vector<std::unique_ptr<Node> >().swap(root_.children);
        root_.value.reset();

        while (head != nullptr) {
            Node* cur = head;
            head = cur->teardown_next;
            while (!cur->children.empty()) {
                Node* child = cur->children.back().release();
                cur->children.pop_back();
                if (child != nullptr) {
                    child->teardown_next = head;
                    head = child;
                }
            }
            delete cur;
        }
        size_ = 0;
    }

    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

private:
    friend class SuffixMatcher<V>;

    struct Node {
        std::string edge;
        std::unique_ptr<V> value;
        std::vector<std::unique_ptr<Node> > children;
        Node* teardown_next;

        Node() noexcept : edge(), value(), children(), teardown_next(nullptr) {}
        explicit Node(std::string e) noexcept
            : edge(std::move(e)), value(), children(), teardown_next(nullptr) {}
    };

    static std::size_t common_prefix_len(const char* a,
                                         std::size_t alen,
                                         const char* b,
                                         std::size_t blen) noexcept {
        const std::size_t n = (alen < blen) ? alen : blen;
        std::size_t i = 0;
        while (i < n && a[i] == b[i]) {
            ++i;
        }
        return i;
    }

    static std::size_t common_prefix_len_reversed(const char* edge,
                                                  std::size_t edge_len,
                                                  const char* str,
                                                  std::size_t str_end_idx,
                                                  std::size_t rem) noexcept {
        const std::size_t n = (edge_len < rem) ? edge_len : rem;
        std::size_t i = 0;
        while (i < n && edge[i] == str[str_end_idx - 1 - i]) {
            ++i;
        }
        return i;
    }

    static std::size_t lower_bound_child(const Node* node, unsigned char byte) noexcept {
        std::size_t lo = 0;
        std::size_t hi = node->children.size();
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            const unsigned char c = static_cast<unsigned char>(node->children[mid]->edge[0]);
            if (c < byte) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return lo;
    }

    static std::size_t find_child_index(const Node* node, unsigned char byte) noexcept {
        const std::size_t idx = lower_bound_child(node, byte);
        if (idx < node->children.size() &&
            static_cast<unsigned char>(node->children[idx]->edge[0]) == byte) {
            return idx;
        }
        return node->children.size();
    }

    bool insert_ptr(const std::string& key, std::unique_ptr<V> val) {
        if (key.empty()) {
            const bool is_new = !root_.value;
            root_.value = std::move(val);
            if (is_new) {
                ++size_;
            }
            return is_new;
        }

        Node* cur = &root_;
        std::size_t pos = 0;

        for (;;) {
            const unsigned char first_byte = static_cast<unsigned char>(key[pos]);
            const std::size_t lb = lower_bound_child(cur, first_byte);
            if (lb == cur->children.size() ||
                static_cast<unsigned char>(cur->children[lb]->edge[0]) != first_byte) {
                std::unique_ptr<Node> leaf(new Node(key.substr(pos)));
                leaf->value = std::move(val);
                cur->children.insert(
                    cur->children.begin() + static_cast<std::ptrdiff_t>(lb),
                    std::move(leaf));
                ++size_;
                return true;
            }

            Node* child = cur->children[lb].get();
            const std::size_t rem = key.size() - pos;
            const std::size_t common = common_prefix_len(
                child->edge.data(), child->edge.size(),
                key.data() + pos, rem);

            if (common == child->edge.size()) {
                pos += common;
                if (pos == key.size()) {
                    const bool is_new = !child->value;
                    child->value = std::move(val);
                    if (is_new) {
                        ++size_;
                    }
                    return is_new;
                }
                cur = child;
                continue;
            }

            // Split `child` at `common` bytes.
            // Prepare ALL allocations before modifying `child` or `cur->children[lb]`.
            std::string split_prefix = child->edge.substr(0, common);
            std::string existing_suffix = child->edge.substr(common);
            std::unique_ptr<Node> split(new Node(std::move(split_prefix)));

            if (common == rem) {
                // New key ends right at the split node.
                split->children.reserve(1);

                // --- Commit point (nothrow from here) ---
                split->value = std::move(val);
                child->edge.swap(existing_suffix);
                split->children.push_back(std::move(cur->children[lb]));
            } else {
                std::string branch_edge = key.substr(pos + common);
                std::unique_ptr<Node> branch(new Node(std::move(branch_edge)));
                split->children.reserve(2);

                const unsigned char existing_b = static_cast<unsigned char>(existing_suffix[0]);
                const unsigned char branch_b = static_cast<unsigned char>(branch->edge[0]);

                // --- Commit point (nothrow from here) ---
                branch->value = std::move(val);
                child->edge.swap(existing_suffix);
                if (existing_b < branch_b) {
                    split->children.push_back(std::move(cur->children[lb]));
                    split->children.push_back(std::move(branch));
                } else {
                    split->children.push_back(std::move(branch));
                    split->children.push_back(std::move(cur->children[lb]));
                }
            }

            cur->children[lb] = std::move(split);
            ++size_;
            return true;
        }
    }

    Node* find_node(const std::string& key) noexcept {
        return const_cast<Node*>(
            static_cast<const RadixTree*>(this)->find_node(key));
    }

    const Node* find_node(const std::string& key) const noexcept {
        const Node* cur = &root_;
        std::size_t pos = 0;
        while (pos < key.size()) {
            const unsigned char first_byte = static_cast<unsigned char>(key[pos]);
            const std::size_t idx = find_child_index(cur, first_byte);
            if (idx == cur->children.size()) {
                return nullptr;
            }
            const Node* child = cur->children[idx].get();
            const std::size_t rem = key.size() - pos;
            if (rem < child->edge.size()) {
                return nullptr;
            }
            const std::size_t common = common_prefix_len(
                child->edge.data(), child->edge.size(),
                key.data() + pos, child->edge.size());
            if (common < child->edge.size()) {
                return nullptr;
            }
            pos += child->edge.size();
            cur = child;
        }
        return cur;
    }

    const Node* find_node_reversed(const std::string& key) const noexcept {
        const Node* cur = &root_;
        std::size_t pos = 0;
        const std::size_t total = key.size();
        while (pos < total) {
            const std::size_t end_idx = total - pos;
            const unsigned char first_byte = static_cast<unsigned char>(key[end_idx - 1]);
            const std::size_t idx = find_child_index(cur, first_byte);
            if (idx == cur->children.size()) {
                return nullptr;
            }
            const Node* child = cur->children[idx].get();
            if (end_idx < child->edge.size()) {
                return nullptr;
            }
            const std::size_t common = common_prefix_len_reversed(
                child->edge.data(), child->edge.size(),
                key.data(), end_idx, child->edge.size());
            if (common < child->edge.size()) {
                return nullptr;
            }
            pos += child->edge.size();
            cur = child;
        }
        return cur;
    }

    V* find_reversed(const std::string& key) noexcept {
        Node* n = const_cast<Node*>(find_node_reversed(key));
        return (n != nullptr && n->value != nullptr) ? n->value.get() : nullptr;
    }

    const V* find_reversed(const std::string& key) const noexcept {
        const Node* n = find_node_reversed(key);
        return (n != nullptr && n->value != nullptr) ? n->value.get() : nullptr;
    }

    const V* longest_prefix_match_reversed(const std::string& text,
                                           std::size_t* matched_len = nullptr) const noexcept {
        const V* best_val = nullptr;
        std::size_t best_len = 0;

        const Node* cur = &root_;
        if (cur->value) {
            best_val = cur->value.get();
            best_len = 0;
        }

        std::size_t pos = 0;
        const std::size_t total = text.size();
        while (pos < total) {
            const std::size_t end_idx = total - pos;
            const unsigned char first_byte = static_cast<unsigned char>(text[end_idx - 1]);
            const std::size_t idx = find_child_index(cur, first_byte);
            if (idx == cur->children.size()) {
                break;
            }
            const Node* child = cur->children[idx].get();
            if (end_idx < child->edge.size()) {
                break;
            }
            const std::size_t common = common_prefix_len_reversed(
                child->edge.data(), child->edge.size(),
                text.data(), end_idx, child->edge.size());
            if (common < child->edge.size()) {
                break;
            }
            pos += child->edge.size();
            cur = child;
            if (cur->value) {
                best_val = cur->value.get();
                best_len = pos;
            }
        }

        if (best_val != nullptr && matched_len != nullptr) {
            *matched_len = best_len;
        }
        return best_val;
    }

    template <typename Fn>
    void dfs_collect(const Node* node,
                     std::string& key_buf,
                     Fn& fn,
                     std::size_t limit,
                     std::size_t& visited) const {
        if (visited >= limit) {
            return;
        }
        if (node->value) {
            fn(key_buf, *node->value);
            ++visited;
            if (visited >= limit) {
                return;
            }
        }
        for (std::size_t i = 0; i < node->children.size() && visited < limit; ++i) {
            const Node* child = node->children[i].get();
            const std::size_t prev_len = key_buf.size();
            key_buf.append(child->edge);
            dfs_collect(child, key_buf, fn, limit, visited);
            key_buf.resize(prev_len);
        }
    }

    Node root_;
    std::size_t size_;
};

// Reverse-key RadixTree for O(L) suffix lookup and longest-suffix matching.
//
// Common ads/search uses:
//   - Domain suffix targeting / routing (e.g. ".example.com" matching "ads.us.example.com")
//   - File extension rules (".tar.gz", ".json", ".png")
//   - Reverse-string suffix dictionary scans
template <typename V>
class SuffixMatcher {
public:
    typedef std::string key_type;
    typedef V mapped_type;

    bool insert(const std::string& suffix, const V& value) {
        return tree_.insert(reverse_str(suffix), value);
    }

    bool insert(const std::string& suffix, V&& value) {
        return tree_.insert(reverse_str(suffix), std::move(value));
    }

    template <typename... Args>
    bool emplace(const std::string& suffix, Args&&... args) {
        return tree_.emplace(reverse_str(suffix), std::forward<Args>(args)...);
    }

    V* find(const std::string& suffix) noexcept {
        return tree_.find_reversed(suffix);
    }

    const V* find(const std::string& suffix) const noexcept {
        return tree_.find_reversed(suffix);
    }

    bool contains(const std::string& suffix) const noexcept {
        return tree_.find_reversed(suffix) != nullptr;
    }

    bool erase(const std::string& suffix) {
        return tree_.erase(reverse_str(suffix));
    }

    // Finds the longest registered suffix that matches the end of `text`.
    const V* longest_suffix_match(const std::string& text,
                                  std::size_t* matched_len = nullptr) const noexcept {
        return tree_.longest_prefix_match_reversed(text, matched_len);
    }

    // Visits up to `limit` registered keys ending with `suffix`.
    // Calls `fn(const std::string& key, const V& value)` with unreversed keys.
    template <typename Fn>
    std::size_t suffix_scan(
        const std::string& suffix,
        Fn&& fn,
        std::size_t limit = std::numeric_limits<std::size_t>::max()) const {
        return tree_.prefix_scan(
            reverse_str(suffix),
            [&fn](const std::string& rev_key, const V& val) {
                fn(reverse_str(rev_key), val);
            },
            limit);
    }

    void clear() noexcept { tree_.clear(); }
    std::size_t size() const noexcept { return tree_.size(); }
    bool empty() const noexcept { return tree_.empty(); }

private:
    static std::string reverse_str(const std::string& s) {
        return std::string(s.rbegin(), s.rend());
    }

    RadixTree<V> tree_;
};

} // namespace search
} // namespace myframe
