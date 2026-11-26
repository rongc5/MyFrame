#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

namespace myframe {
namespace search {

// Fixed-capacity O(N log K) Top-K selector backed by a bounded binary heap.
//
// Common ads/search uses:
//   - Selecting top-K ads by eCPM / bid during ranking
//   - Selecting top-K search documents by BM25 / vector score during recall
//   - Zero heap allocations after construction (storage reserved up front)
//
// Comparator convention (matches std::priority_queue):
//   `Compare(a, b)` must be a strict weak ordering returning true if `a` is
//   strictly WORSE than `b` (NaN floating-point keys violate strict weak
//   ordering and must be filtered out or handled in a custom comparator).
//   - Default `std::less<T>`: retains the K LARGEST elements (best = largest);
//     extract_sorted() returns them in descending order (best first).
//   - `std::greater<T>`: retains the K SMALLEST elements (best = smallest);
//     extract_sorted() returns them in ascending order (best first).
template <typename T, typename Compare = std::less<T> >
class TopK {
public:
    typedef T value_type;

    explicit TopK(std::size_t k, const Compare& comp = Compare())
        : capacity_(k), comp_(comp), heap_() {
        heap_.reserve(k);
    }

    // Attempts to insert `item` into the top-K set.
    // Returns true if `item` was accepted, false if it was worse than the
    // current worst element (or if capacity == 0).
    bool push(const T& item) {
        if (capacity_ == 0) {
            return false;
        }
        assert(!comp_(item, item)); // Catch invalid/NaN strict-weak-order violations
        if (heap_.size() < capacity_) {
            heap_.push_back(item);
            sift_up(heap_.size() - 1);
            return true;
        }
        if (!comp_(heap_[0], item)) {
            return false;
        }
        heap_[0] = item;
        sift_down(0);
        return true;
    }

    bool push(T&& item) {
        if (capacity_ == 0) {
            return false;
        }
        assert(!comp_(item, item)); // Catch invalid/NaN strict-weak-order violations
        if (heap_.size() < capacity_) {
            heap_.push_back(std::move(item));
            sift_up(heap_.size() - 1);
            return true;
        }
        if (!comp_(heap_[0], item)) {
            return false;
        }
        heap_[0] = std::move(item);
        sift_down(0);
        return true;
    }

    template <typename... Args>
    bool emplace(Args&&... args) {
        if (capacity_ == 0) {
            return false;
        }
        return push(T(std::forward<Args>(args)...));
    }

    // Returns the current worst element inside the top-K set (the threshold to beat).
    // Precondition: !empty().
    const T& worst() const noexcept {
        assert(!heap_.empty());
        return heap_[0];
    }

    // Moves elements out in best-first sorted order and clears this collector.
    // Pre-reserves the fresh backing vector before swapping so a reserve
    // allocation failure leaves the existing elements untouched.
    std::vector<T> extract_sorted() {
        std::vector<T> out;
        out.reserve(capacity_);
        out.swap(heap_);
        sort_best_first(out);
        return out;
    }

    // Non-destructive copy in best-first sorted order.
    std::vector<T> sorted_copy() const {
        std::vector<T> out = heap_;
        sort_best_first(out);
        return out;
    }

    void clear() noexcept { heap_.clear(); }
    std::size_t size() const noexcept { return heap_.size(); }
    std::size_t capacity() const noexcept { return capacity_; }
    bool empty() const noexcept { return heap_.empty(); }
    bool full() const noexcept { return heap_.size() == capacity_; }

private:
    // Internal heap invariant: parent is WORSE than or equal to children
    // (i.e. !comp_(child, parent)), so heap_[0] is always the worst element.
    void sift_up(std::size_t idx) {
        using std::swap;
        while (idx > 0) {
            const std::size_t parent = (idx - 1) / 2;
            if (comp_(heap_[idx], heap_[parent])) {
                swap(heap_[idx], heap_[parent]);
                idx = parent;
            } else {
                break;
            }
        }
    }

    void sift_down(std::size_t idx) {
        using std::swap;
        const std::size_t n = heap_.size();
        for (;;) {
            const std::size_t left = idx * 2 + 1;
            if (left >= n) {
                break;
            }
            std::size_t worst_child = left;
            const std::size_t right = left + 1;
            if (right < n && comp_(heap_[right], heap_[left])) {
                worst_child = right;
            }
            if (comp_(heap_[worst_child], heap_[idx])) {
                swap(heap_[idx], heap_[worst_child]);
                idx = worst_child;
            } else {
                break;
            }
        }
    }

    void sort_best_first(std::vector<T>& items) const {
        const Compare& comp = comp_;
        std::sort(items.begin(), items.end(),
                  [&comp](const T& a, const T& b) {
                      return comp(b, a); // Best first: a > b under comp
                  });
    }

    std::size_t capacity_;
    Compare comp_;
    std::vector<T> heap_;
};

} // namespace search
} // namespace myframe
