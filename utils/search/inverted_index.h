#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace myframe {
namespace search {

// In-memory inverted index with sorted posting lists and fast Boolean recall.
//
// Common ads/search uses:
//   - Search term recall: multi-term AND / OR / AND-NOT postings retrieval
//   - Ad targeting recall: matching (geo, os, interest, crowd) targeting predicates
//   - Hot-swappable offline index snapshot when paired with DoubleBuffer<InvertedIndex>
//
// Two build modes:
//   1. Bulk build (fastest for reload): call add_unsorted(...) repeatedly, then finalize().
//   2. Incremental: call add(...) / remove(...) directly (keeps lists sorted & deduplicated).
template <typename Term,
          typename DocId = std::uint64_t,
          typename Hash = std::hash<Term>,
          typename KeyEqual = std::equal_to<Term> >
class InvertedIndex {
public:
    typedef Term term_type;
    typedef DocId doc_id_type;
    typedef std::vector<DocId> PostingList;

    explicit InvertedIndex(const Hash& hash = Hash(),
                           const KeyEqual& eq = KeyEqual())
        : postings_(0, hash, eq), finalized_(true) {}

    // -------------------------------------------------------------------------
    // Bulk build API (fastest for offline/background index construction)
    // -------------------------------------------------------------------------

    void add_unsorted(const Term& term, DocId doc_id) {
        postings_[term].push_back(doc_id);
        finalized_ = false;
    }

    void finalize() {
        for (typename MapType::iterator it = postings_.begin();
             it != postings_.end();
             ++it) {
            PostingList& list = it->second;
            std::sort(list.begin(), list.end());
            list.erase(std::unique(list.begin(), list.end()), list.end());
        }
        finalized_ = true;
    }

    bool finalized() const noexcept { return finalized_; }

    // -------------------------------------------------------------------------
    // Incremental update API (keeps lists sorted and deduplicated)
    // -------------------------------------------------------------------------

    // Returns true if (term, doc_id) was newly added, false if already present.
    bool add(const Term& term, DocId doc_id) {
        ensure_finalized();
        typename MapType::iterator map_it = postings_.find(term);
        if (map_it == postings_.end()) {
            PostingList fresh;
            fresh.push_back(doc_id);
            postings_.emplace(term, std::move(fresh));
            return true;
        }
        PostingList& list = map_it->second;
        if (list.empty() || list.back() < doc_id) {
            list.push_back(doc_id);
            return true;
        }
        typename PostingList::iterator it =
            std::lower_bound(list.begin(), list.end(), doc_id);
        if (it != list.end() && *it == doc_id) {
            return false;
        }
        list.insert(it, doc_id);
        return true;
    }

    bool remove(const Term& term, DocId doc_id) {
        ensure_finalized();
        typename MapType::iterator map_it = postings_.find(term);
        if (map_it == postings_.end()) {
            return false;
        }
        PostingList& list = map_it->second;
        typename PostingList::iterator it =
            std::lower_bound(list.begin(), list.end(), doc_id);
        if (it == list.end() || *it != doc_id) {
            return false;
        }
        list.erase(it);
        if (list.empty()) {
            postings_.erase(map_it);
        }
        return true;
    }

    bool remove_term(const Term& term) {
        return postings_.erase(term) > 0;
    }

    // -------------------------------------------------------------------------
    // Posting lookup & Boolean set algebra
    // -------------------------------------------------------------------------

    const PostingList* postings(const Term& term) const {
        ensure_finalized();
        typename MapType::const_iterator it = postings_.find(term);
        return (it != postings_.end()) ? &it->second : nullptr;
    }

    // Intersects two sorted, deduplicated posting lists.
    // Automatically uses galloping (exponential + binary) search when list lengths
    // are skewed (>= 8x), and linear two-pointer merge otherwise.
    static PostingList intersect_lists(const PostingList& a, const PostingList& b) {
        if (a.empty() || b.empty()) {
            return PostingList();
        }
        const PostingList& shorter = (a.size() <= b.size()) ? a : b;
        const PostingList& longer = (a.size() <= b.size()) ? b : a;

        PostingList out;
        out.reserve(shorter.size());

        if (longer.size() >= shorter.size() * 8) {
            std::size_t pos = 0;
            const std::size_t n = longer.size();
            for (std::size_t i = 0; i < shorter.size() && pos < n; ++i) {
                const DocId target = shorter[i];
                pos = gallop_lower_bound(longer, pos, n, target);
                if (pos < n && longer[pos] == target) {
                    out.push_back(target);
                    ++pos;
                }
            }
        } else {
            std::size_t i = 0;
            std::size_t j = 0;
            while (i < shorter.size() && j < longer.size()) {
                if (shorter[i] < longer[j]) {
                    ++i;
                } else if (longer[j] < shorter[i]) {
                    ++j;
                } else {
                    out.push_back(shorter[i]);
                    ++i;
                    ++j;
                }
            }
        }
        return out;
    }

    // Union of two sorted, deduplicated posting lists.
    static PostingList unite_lists(const PostingList& a, const PostingList& b) {
        PostingList out;
        out.reserve(a.size() + b.size());
        std::size_t i = 0;
        std::size_t j = 0;
        while (i < a.size() && j < b.size()) {
            if (a[i] < b[j]) {
                out.push_back(a[i++]);
            } else if (b[j] < a[i]) {
                out.push_back(b[j++]);
            } else {
                out.push_back(a[i]);
                ++i;
                ++j;
            }
        }
        while (i < a.size()) out.push_back(a[i++]);
        while (j < b.size()) out.push_back(b[j++]);
        return out;
    }

    // Set difference: elements in `include` that are NOT in `excluded` (both sorted).
    static PostingList exclude_list(const PostingList& include,
                                    const PostingList& excluded) {
        if (include.empty() || excluded.empty()) {
            return include;
        }
        PostingList out;
        out.reserve(include.size());

        if (excluded.size() >= include.size() * 8) {
            std::size_t pos = 0;
            const std::size_t n = excluded.size();
            for (std::size_t i = 0; i < include.size(); ++i) {
                const DocId target = include[i];
                pos = gallop_lower_bound(excluded, pos, n, target);
                if (pos < n && excluded[pos] == target) {
                    ++pos;
                } else {
                    out.push_back(target);
                }
            }
        } else {
            std::size_t i = 0;
            std::size_t j = 0;
            while (i < include.size() && j < excluded.size()) {
                if (include[i] < excluded[j]) {
                    out.push_back(include[i++]);
                } else if (excluded[j] < include[i]) {
                    ++j;
                } else {
                    ++i;
                    ++j;
                }
            }
            while (i < include.size()) {
                out.push_back(include[i++]);
            }
        }
        return out;
    }

    // Multi-term AND recall with optional AND-NOT exclusions.
    // Processes rarest posting lists first for minimal intermediate work.
    PostingList query_and(
        const std::vector<Term>& must_terms,
        const std::vector<Term>& must_not_terms = std::vector<Term>()) const {
        ensure_finalized();
        if (must_terms.empty()) {
            return PostingList();
        }

        std::vector<const PostingList*> lists;
        lists.reserve(must_terms.size());
        for (std::size_t i = 0; i < must_terms.size(); ++i) {
            const PostingList* pl = postings(must_terms[i]);
            if (pl == nullptr || pl->empty()) {
                return PostingList();
            }
            lists.push_back(pl);
        }

        std::sort(lists.begin(), lists.end(),
                  [](const PostingList* a, const PostingList* b) {
                      if (a->size() != b->size()) {
                          return a->size() < b->size();
                      }
                      return std::less<const PostingList*>()(a, b);
                  });
        lists.erase(std::unique(lists.begin(), lists.end()), lists.end());

        PostingList acc = *lists[0];
        for (std::size_t i = 1; i < lists.size() && !acc.empty(); ++i) {
            acc = intersect_lists(acc, *lists[i]);
        }

        apply_exclusions(acc, must_not_terms);
        return acc;
    }

    // Multi-term OR recall with optional AND-NOT exclusions.
    // Uses k-way min-heap merge when > 2 lists are present.
    PostingList query_or(
        const std::vector<Term>& should_terms,
        const std::vector<Term>& must_not_terms = std::vector<Term>()) const {
        ensure_finalized();
        if (should_terms.empty()) {
            return PostingList();
        }

        std::vector<const PostingList*> lists;
        lists.reserve(should_terms.size());
        for (std::size_t i = 0; i < should_terms.size(); ++i) {
            const PostingList* pl = postings(should_terms[i]);
            if (pl != nullptr && !pl->empty()) {
                lists.push_back(pl);
            }
        }
        if (lists.empty()) {
            return PostingList();
        }

        PostingList acc;
        if (lists.size() == 1) {
            acc = *lists[0];
        } else if (lists.size() == 2) {
            acc = unite_lists(*lists[0], *lists[1]);
        } else {
            acc = kway_unite(lists);
        }

        apply_exclusions(acc, must_not_terms);
        return acc;
    }

    void clear() noexcept {
        postings_.clear();
        finalized_ = true;
    }

    std::size_t term_count() const noexcept { return postings_.size(); }
    bool empty() const noexcept { return postings_.empty(); }

private:
    typedef std::unordered_map<Term, PostingList, Hash, KeyEqual> MapType;

    void ensure_finalized() const {
        if (!finalized_) {
            throw std::logic_error(
                "InvertedIndex::finalize() must be called after add_unsorted()");
        }
    }

    static std::size_t gallop_lower_bound(const PostingList& list,
                                          std::size_t lo,
                                          std::size_t hi,
                                          DocId target) noexcept {
        if (lo >= hi || !(list[lo] < target)) {
            return lo;
        }
        std::size_t step = 1;
        std::size_t bound = lo + step;
        while (bound < hi && list[bound] < target) {
            lo = bound;
            step <<= 1;
            bound = (step < hi - lo) ? (lo + step) : hi;
        }
        // Binary search in (lo, bound]
        std::size_t left = lo + 1;
        std::size_t right = bound;
        while (left < right) {
            const std::size_t mid = left + (right - left) / 2;
            if (list[mid] < target) {
                left = mid + 1;
            } else {
                right = mid;
            }
        }
        return left;
    }

    static PostingList kway_unite(const std::vector<const PostingList*>& lists) {
        struct Cursor {
            DocId doc;
            std::size_t list_idx;
            std::size_t elem_idx;
        };
        struct MinDoc {
            bool operator()(const Cursor& a, const Cursor& b) const noexcept {
                return b.doc < a.doc; // Min-heap by doc id
            }
        };

        std::priority_queue<Cursor, std::vector<Cursor>, MinDoc> pq;
        std::size_t total_hint = 0;
        for (std::size_t i = 0; i < lists.size(); ++i) {
            total_hint += lists[i]->size();
            pq.push(Cursor{(*lists[i])[0], i, 0});
        }

        PostingList out;
        out.reserve(total_hint);
        bool has_last = false;
        DocId last_doc = DocId();

        while (!pq.empty()) {
            const Cursor top = pq.top();
            pq.pop();
            if (!has_last || last_doc < top.doc) {
                out.push_back(top.doc);
                last_doc = top.doc;
                has_last = true;
            }
            const std::size_t next_idx = top.elem_idx + 1;
            if (next_idx < lists[top.list_idx]->size()) {
                pq.push(Cursor{(*lists[top.list_idx])[next_idx], top.list_idx, next_idx});
            }
        }
        return out;
    }

    void apply_exclusions(PostingList& acc,
                          const std::vector<Term>& must_not_terms) const {
        for (std::size_t i = 0; i < must_not_terms.size() && !acc.empty(); ++i) {
            const PostingList* ex = postings(must_not_terms[i]);
            if (ex != nullptr && !ex->empty()) {
                acc = exclude_list(acc, *ex);
            }
        }
    }

    MapType postings_;
    bool finalized_;
};

} // namespace search
} // namespace myframe
