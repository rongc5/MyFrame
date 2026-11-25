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

// Compressed prefix tree (Radix Tree / Patricia Trie) for byte strings (UTF-8 safe).
//
// Common ads/search uses:
//   - Query suggestion / auto-complete via prefix_scan(prefix, fn, limit)
//   - URL / category / routing rules via longest_prefix_match(text, ...)
//   - Compact ordered dictionary with automatic edge splitting and erase coalescing
//
// Properties:
//   - Strict C++11, header-only, supports move-only and non-default-constructible V.
//   - Children sorted by unsigned first byte: compact memory (no 256-pointer blowup
//     per node) and deterministic lexicographical prefix_scan order.
//   - Iterative destructor prevents stack overflow even on deeply chained keys.
template <typename V>
class RadixTree {
public:
    typedef std::string key_type;
    typedef V mapped_type;

    RadixTree() : root_(new Node()), size_(0) {}

    ~RadixTree() noexcept {
        clear();
    }

    RadixTree(const RadixTree&) = delete;
    RadixTree& operator=(const RadixTree&) = delete;

    RadixTree(RadixTree&& other) noexcept
        : root_(std::move(other.root_)), size_(other.size_) {
        other.root_.reset(new Node());
        other.size_ = 0;
    }

    RadixTree& operator=(RadixTree&& other) noexcept {
        if (this != &other) {
            clear();
            root_ = std::move(other.root_);
            size_ = other.size_;
            other.root_.reset(new Node());
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
    bool erase(const std::string& key) {
        if (key.empty()) {
            if (!root_->value) {
                return false;
            }
            root_->value.reset();
            --size_;
            return true;
        }

        struct PathStep {
            Node* parent;
            std::size_t child_idx;
        };
        std::vector<PathStep> path;

        Node* cur = root_.get();
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
        cur->value.reset();
        --size_;

        // Walk back up the path removing dead leaves and coalescing single-child
        // non-terminal internal nodes.
        while (!path.empty()) {
            const PathStep step = path.back();
            path.pop_back();
            Node* node = step.parent->children[step.child_idx].get();
            if (!node->value && node->children.empty()) {
                step.parent->children.erase(
                    step.parent->children.begin() +
                    static_cast<std::ptrdiff_t>(step.child_idx));
            } else if (!node->value && node->children.size() == 1) {
                std::unique_ptr<Node> only_child = std::move(node->children[0]);
                only_child->edge.insert(0, node->edge);
                step.parent->children[step.child_idx] = std::move(only_child);
                break;
            } else {
                break;
            }
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

        const Node* cur = root_.get();
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

        const Node* cur = root_.get();
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

    void clear() noexcept {
        if (!root_) {
            return;
        }
        // Iterative teardown avoids deep call stacks on pathological trees.
        std::vector<std::unique_ptr<Node> > stack;
        for (std::size_t i = 0; i < root_->children.size(); ++i) {
            stack.push_back(std::move(root_->children[i]));
        }
        root_->children.clear();
        root_->value.reset();
        while (!stack.empty()) {
            std::unique_ptr<Node> node = std::move(stack.back());
            stack.pop_back();
            for (std::size_t i = 0; i < node->children.size(); ++i) {
                stack.push_back(std::move(node->children[i]));
            }
            node->children.clear();
        }
        size_ = 0;
    }

    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

private:
    struct Node {
        std::string edge;
        std::unique_ptr<V> value;
        std::vector<std::unique_ptr<Node> > children;

        Node() : edge(), value(), children() {}
        explicit Node(std::string e) : edge(std::move(e)), value(), children() {}
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
            const bool is_new = !root_->value;
            root_->value = std::move(val);
            if (is_new) {
                ++size_;
            }
            return is_new;
        }

        Node* cur = root_.get();
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
            std::unique_ptr<Node> split(new Node(child->edge.substr(0, common)));
            std::unique_ptr<Node> existing = std::move(cur->children[lb]);
            existing->edge.erase(0, common);

            if (common == rem) {
                // New key ends right at the split node.
                split->value = std::move(val);
                split->children.push_back(std::move(existing));
            } else {
                std::unique_ptr<Node> branch(new Node(key.substr(pos + common)));
                branch->value = std::move(val);
                const unsigned char existing_b = static_cast<unsigned char>(existing->edge[0]);
                const unsigned char branch_b = static_cast<unsigned char>(branch->edge[0]);
                if (existing_b < branch_b) {
                    split->children.push_back(std::move(existing));
                    split->children.push_back(std::move(branch));
                } else {
                    split->children.push_back(std::move(branch));
                    split->children.push_back(std::move(existing));
                }
            }

            cur->children[lb] = std::move(split);
            ++size_;
            return true;
        }
    }

    Node* find_node(const std::string& key) const noexcept {
        Node* cur = root_.get();
        std::size_t pos = 0;
        while (pos < key.size()) {
            const unsigned char first_byte = static_cast<unsigned char>(key[pos]);
            const std::size_t idx = find_child_index(cur, first_byte);
            if (idx == cur->children.size()) {
                return nullptr;
            }
            Node* child = cur->children[idx].get();
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

    std::unique_ptr<Node> root_;
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
        return tree_.find(reverse_str(suffix));
    }

    const V* find(const std::string& suffix) const noexcept {
        return tree_.find(reverse_str(suffix));
    }

    bool contains(const std::string& suffix) const noexcept {
        return tree_.contains(reverse_str(suffix));
    }

    bool erase(const std::string& suffix) {
        return tree_.erase(reverse_str(suffix));
    }

    // Finds the longest registered suffix that matches the end of `text`.
    const V* longest_suffix_match(const std::string& text,
                                  std::size_t* matched_len = nullptr) const noexcept {
        return tree_.longest_prefix_match(reverse_str(text), matched_len);
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
