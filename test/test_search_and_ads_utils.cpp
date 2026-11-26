#include "search/search.h"
#include "containers/containers.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

thread_local bool g_alloc_tracking = false;
thread_local bool g_alloc_fail_armed = false;
thread_local std::size_t g_alloc_fail_after = 0;
thread_local std::size_t g_alloc_calls = 0;

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

namespace {

using myframe::containers::ConsistentHash;
using myframe::containers::DoubleBuffer;
using myframe::containers::ShardedRateLimiter;
using myframe::containers::ShardedTtlCache;
using myframe::containers::TokenBucket;
using myframe::search::AhoCorasick;
using myframe::search::BloomFilter;
using myframe::search::InvertedIndex;
using myframe::search::RadixTree;
using myframe::search::SuffixMatcher;
using myframe::search::TopK;

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

// Deterministic monotonic test clock (zero sleep, zero timing flakiness).
struct FakeClock {
    typedef std::chrono::milliseconds duration;
    typedef duration::rep rep;
    typedef duration::period period;
    typedef std::chrono::time_point<FakeClock, duration> time_point;
    static const bool is_steady = true;

    static std::atomic<std::int64_t> now_ms;

    static time_point now() noexcept {
        return time_point(duration(now_ms.load(std::memory_order_acquire)));
    }
    static void set_ms(std::int64_t ms) noexcept {
        now_ms.store(ms, std::memory_order_release);
    }
    static void advance_ms(std::int64_t delta) noexcept {
        now_ms.fetch_add(delta, std::memory_order_acq_rel);
    }
};
std::atomic<std::int64_t> FakeClock::now_ms(1000);

// ---------------------------------------------------------------------------
// 1. RadixTree & SuffixMatcher (differential test vs std::map + UTF-8)
// ---------------------------------------------------------------------------

void test_radix_and_suffix() {
    // Basic edge splits, empty key, LPM, prefix_scan, erase coalescing
    {
        RadixTree<int> tree;
        CHECK(tree.empty() && tree.size() == 0);
        CHECK(tree.find("") == nullptr);
        CHECK(tree.longest_prefix_match("anything") == nullptr);

        CHECK(tree.insert("", 0));
        CHECK(!tree.insert("", 99));
        CHECK(*tree.find("") == 99);

        CHECK(tree.insert("app", 1));
        CHECK(tree.insert("apple", 2));
        CHECK(tree.insert("application", 3));
        CHECK(tree.insert("apply", 4));
        CHECK(tree.insert("apricot", 5));
        CHECK(tree.insert("banana", 6));
        CHECK(tree.size() == 7);

        // Longest prefix match
        std::size_t mlen = 999;
        const int* v = tree.longest_prefix_match("applications_v2", &mlen);
        CHECK(v != nullptr && *v == 3 && mlen == 11);

        v = tree.longest_prefix_match("applet", &mlen);
        CHECK(v != nullptr && *v == 2 && mlen == 5);

        v = tree.longest_prefix_match("zzz", &mlen);
        CHECK(v != nullptr && *v == 99 && mlen == 0); // Fallback to empty key

        // Prefix scan ending mid-edge ("appl") with limit
        std::vector<std::string> got;
        std::size_t cnt = tree.prefix_scan(
            "appl",
            [&got](const std::string& k, const int&) { got.push_back(k); },
            2);
        CHECK(cnt == 2 && got.size() == 2);
        CHECK(got[0] == "apple" && got[1] == "application");

        // Erase triggering node coalescing
        CHECK(tree.erase("application"));
        CHECK(!tree.erase("application"));
        CHECK(tree.erase("apply"));
        CHECK(tree.erase(""));
        CHECK(tree.longest_prefix_match("zzz") == nullptr);
        CHECK(tree.find("apple") != nullptr && *tree.find("apple") == 2);
        CHECK(tree.find("app") != nullptr && *tree.find("app") == 1);
    }

    // Differential test against std::map (unsigned byte order)
    {
        struct UnsignedLess {
            bool operator()(const std::string& a, const std::string& b) const {
                const std::size_t n = std::min(a.size(), b.size());
                for (std::size_t i = 0; i < n; ++i) {
                    const unsigned char ca = static_cast<unsigned char>(a[i]);
                    const unsigned char cb = static_cast<unsigned char>(b[i]);
                    if (ca != cb) return ca < cb;
                }
                return a.size() < b.size();
            }
        };

        RadixTree<int> tree;
        std::map<std::string, int, UnsignedLess> oracle;

        // Pool mixing ASCII prefixes and Chinese UTF-8 search queries
        std::vector<std::string> keys = {
            "", "a", "ab", "abc", "abd", "ac", "b", "ba", "bb",
            "/api", "/api/v1", "/api/v1/ads", "/api/v1/search", "/api/v2/ads",
            "搜索", "搜索引擎", "搜索广告", "搜索推荐", "广告", "广告投放", "广告主"
        };
        std::mt19937 rng(42);
        const char alphabet[] = "abcd01/";
        for (int i = 0; i < 300; ++i) {
            std::size_t len = (rng() % 12) + 1;
            std::string s;
            s.reserve(len);
            for (std::size_t j = 0; j < len; ++j) {
                s.push_back(alphabet[rng() % (sizeof(alphabet) - 1)]);
            }
            keys.push_back(s);
        }

        for (int op = 0; op < 1500; ++op) {
            const std::string& k = keys[rng() % keys.size()];
            const int action = static_cast<int>(rng() % 10);
            if (action < 7) {
                const int val = op + 1;
                const bool inserted_tree = tree.insert(k, val);
                const bool existed_oracle = oracle.count(k) > 0;
                oracle[k] = val;
                CHECK(inserted_tree == !existed_oracle);
            } else {
                const bool erased_tree = tree.erase(k);
                const bool erased_oracle = oracle.erase(k) > 0;
                CHECK(erased_tree == erased_oracle);
            }
        }

        CHECK(tree.size() == oracle.size());

        // Verify every key lookup & prefix_scan against oracle
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const int* tv = tree.find(keys[i]);
            std::map<std::string, int, UnsignedLess>::const_iterator it =
                oracle.find(keys[i]);
            if (it == oracle.end()) {
                CHECK(tv == nullptr);
            } else {
                CHECK(tv != nullptr && *tv == it->second);
            }
        }

        const char* prefixes[] = {"", "a", "ab", "/api", "/api/v1", "搜索", "广告", "zzz"};
        for (std::size_t pi = 0; pi < sizeof(prefixes) / sizeof(prefixes[0]); ++pi) {
            const std::string p(prefixes[pi]);
            std::vector<std::pair<std::string, int> > tree_out;
            std::vector<std::pair<std::string, int> > oracle_out;
            tree.prefix_scan(p, [&](const std::string& k, const int& v) {
                tree_out.push_back(std::make_pair(k, v));
            });
            for (std::map<std::string, int, UnsignedLess>::const_iterator it = oracle.begin();
                 it != oracle.end();
                 ++it) {
                if (it->first.size() >= p.size() &&
                    it->first.compare(0, p.size(), p) == 0) {
                    oracle_out.push_back(std::make_pair(it->first, it->second));
                }
            }
            CHECK(tree_out == oracle_out);
        }

        // Verify longest_prefix_match against oracle
        const char* probes[] = {
            "/api/v1/ads/bid", "/api/v1/unknown", "搜索广告召回", "abcde", "unmatched"
        };
        for (std::size_t qi = 0; qi < sizeof(probes) / sizeof(probes[0]); ++qi) {
            const std::string q(probes[qi]);
            std::size_t got_len = 0;
            const int* got_val = tree.longest_prefix_match(q, &got_len);

            const int* best_val = nullptr;
            std::size_t best_len = 0;
            for (std::map<std::string, int, UnsignedLess>::const_iterator it = oracle.begin();
                 it != oracle.end();
                 ++it) {
                if (it->first.size() <= q.size() &&
                    q.compare(0, it->first.size(), it->first) == 0) {
                    if (best_val == nullptr || it->first.size() >= best_len) {
                        best_val = &it->second;
                        best_len = it->first.size();
                    }
                }
            }
            if (best_val == nullptr) {
                CHECK(got_val == nullptr);
            } else {
                CHECK(got_val != nullptr && *got_val == *best_val && got_len == best_len);
            }
        }
    }

    // Move-only values + deep chain iterative destructor
    {
        RadixTree<std::unique_ptr<int> > mtree;
        std::string deep;
        for (int i = 1; i <= 500; ++i) {
            deep.push_back('x');
            CHECK(mtree.insert(deep, std::unique_ptr<int>(new int(i))));
        }
        CHECK(mtree.size() == 500);
        std::unique_ptr<int>* p = mtree.find(deep);
        CHECK(p != nullptr && **p == 500);
    }

    // SuffixMatcher: domain routing + extensions + UTF-8 Chinese suffixes
    {
        SuffixMatcher<std::string> sm;
        CHECK(sm.insert(".com", "tld_com"));
        CHECK(sm.insert(".example.com", "root_example"));
        CHECK(sm.insert(".ads.example.com", "ads_cluster"));
        CHECK(sm.insert(".tar.gz", "archive_targz"));
        CHECK(sm.insert(".gz", "archive_gz"));
        CHECK(sm.insert("广告", "generic_ad"));
        CHECK(sm.insert("搜索广告", "search_ad"));

        std::size_t mlen = 0;
        const std::string* r = sm.longest_suffix_match("bidder1.ads.example.com", &mlen);
        CHECK(r != nullptr && *r == "ads_cluster" && mlen == 16);

        r = sm.longest_suffix_match("www.example.com", &mlen);
        CHECK(r != nullptr && *r == "root_example");

        r = sm.longest_suffix_match("other.com", &mlen);
        CHECK(r != nullptr && *r == "tld_com");

        r = sm.longest_suffix_match("dump.tar.gz", &mlen);
        CHECK(r != nullptr && *r == "archive_targz");

        r = sm.longest_suffix_match("移动端搜索广告", &mlen);
        CHECK(r != nullptr && *r == "search_ad");

        std::set<std::string> scanned;
        sm.suffix_scan(".com", [&scanned](const std::string& k, const std::string&) {
            scanned.insert(k);
        });
        CHECK(scanned.size() == 3);
        CHECK(scanned.count(".com") == 1 &&
              scanned.count(".example.com") == 1 &&
              scanned.count(".ads.example.com") == 1);
    }
}

// ---------------------------------------------------------------------------
// 2. AhoCorasick (differential test vs brute-force substring scan + UTF-8)
// ---------------------------------------------------------------------------

void test_aho_corasick() {
    // Error guards: empty pattern & scan-before-build
    {
        AhoCorasick ac;
        bool threw = false;
        try {
            ac.add_pattern("");
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);

        ac.add_pattern("abc", 1);
        threw = false;
        try {
            (void)ac.contains_any("abc");
        } catch (const std::logic_error&) {
            threw = true;
        }
        CHECK(threw);
    }

    // Classic overlapping & nested dict_link case + duplicate pattern with 2 tags + early stop
    {
        AhoCorasick ac;
        const std::uint32_t p0 = ac.add_pattern("he", 10);
        const std::uint32_t p1 = ac.add_pattern("she", 20);
        const std::uint32_t p2 = ac.add_pattern("his", 30);
        const std::uint32_t p3 = ac.add_pattern("hers", 40);
        const std::uint32_t p4 = ac.add_pattern("e", 50);
        const std::uint32_t p5 = ac.add_pattern("he", 60); // duplicate pattern, different tag
        (void)p0; (void)p1; (void)p2; (void)p3; (void)p4; (void)p5;
        ac.build();
        CHECK(ac.built() && ac.pattern_count() == 6);

        CHECK(ac.contains_any("ushers"));
        CHECK(!ac.contains_any("xyz"));

        int seen = 0;
        ac.scan_until("ushers", [&seen](const AhoCorasick::Match&) -> bool {
            ++seen;
            return seen < 2; // stop after 2nd match
        });
        CHECK(seen == 2);
    }

    // Differential test vs naive std::string::find on ASCII + Chinese UTF-8 patterns
    {
        std::vector<std::pair<std::string, std::uint64_t> > dict = {
            {"a", 1}, {"ab", 2}, {"abc", 3}, {"bc", 4}, {"c", 5},
            {"aaa", 6}, {"aa", 7}, {"she", 8}, {"he", 9}, {"her", 10},
            {"hers", 11}, {"最高级", 100}, {"国家级", 101}, {"绝对", 102},
            {"绝对有效", 103}, {"有效", 104}, {"第一", 105}, {"第一品牌", 106},
            {"品牌", 107}, {"绝对", 108} // duplicate UTF-8 pattern with second tag
        };

        AhoCorasick ac;
        for (std::size_t i = 0; i < dict.size(); ++i) {
            ac.add_pattern(dict[i].first, dict[i].second);
        }
        ac.build();

        std::vector<std::string> texts = {
            "",
            "ushers abcabcaaaa",
            "本产品是国家级第一品牌，绝对有效，堪称最高级体验！",
            "绝对没有问题，但绝对有效和第一品牌都很有效",
            "zzzzzzzzzz"
        };

        for (std::size_t ti = 0; ti < texts.size(); ++ti) {
            const std::string& text = texts[ti];
            std::vector<AhoCorasick::Match> ac_matches = ac.find_all(text);

            typedef std::pair<std::pair<std::uint32_t, std::uint64_t>,
                              std::pair<std::size_t, std::size_t> >
                MatchKey;
            std::set<MatchKey> got_set;
            for (std::size_t i = 0; i < ac_matches.size(); ++i) {
                const AhoCorasick::Match& m = ac_matches[i];
                CHECK(m.start + m.length <= text.size());
                CHECK(text.compare(m.start, m.length, dict[m.pattern_id].first) == 0);
                got_set.insert(std::make_pair(
                    std::make_pair(m.pattern_id, m.tag),
                    std::make_pair(m.start, m.length)));
            }
            CHECK(got_set.size() == ac_matches.size()); // No duplicate emissions

            std::set<MatchKey> expected_set;
            for (std::size_t pid = 0; pid < dict.size(); ++pid) {
                const std::string& pat = dict[pid].first;
                std::size_t pos = text.find(pat, 0);
                while (pos != std::string::npos) {
                    expected_set.insert(std::make_pair(
                        std::make_pair(static_cast<std::uint32_t>(pid), dict[pid].second),
                        std::make_pair(pos, pat.size())));
                    pos = text.find(pat, pos + 1);
                }
            }
            CHECK(got_set == expected_set);
        }
    }
}

// ---------------------------------------------------------------------------
// 3. BloomFilter (0 false negatives, FP rate bound, merge, serialize)
// ---------------------------------------------------------------------------

void test_bloom_filter() {
    bool threw = false;
    try { BloomFilter bf(0, 0.01); (void)bf; } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { BloomFilter bf(100, 0.0); (void)bf; } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { BloomFilter bf(100, 1.0); (void)bf; } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);

    const std::size_t kItems = 20000;
    BloomFilter bf(kItems, 0.01);
    CHECK(bf.bit_count() >= 64 && (bf.bit_count() % 64) == 0);
    CHECK(bf.hash_count() >= 1 && bf.hash_count() <= 32);

    for (std::size_t i = 0; i < kItems; ++i) {
        bf.add("ad_imp_" + std::to_string(i));
    }
    // Zero false negatives guaranteed
    for (std::size_t i = 0; i < kItems; ++i) {
        CHECK(bf.possibly_contains("ad_imp_" + std::to_string(i)));
    }

    // Empirical FP rate on 30,000 unseen keys (target 1%, require < 2%)
    std::size_t false_positives = 0;
    const std::size_t kProbes = 30000;
    for (std::size_t i = 0; i < kProbes; ++i) {
        if (bf.possibly_contains("unseen_user_" + std::to_string(i + 1000000))) {
            ++false_positives;
        }
    }
    const double fp_rate = static_cast<double>(false_positives) / static_cast<double>(kProbes);
    CHECK(fp_rate < 0.02);

    // Serialize + deserialize round-trip + corruption check
    const std::string blob = bf.serialize();
    BloomFilter restored = BloomFilter::with_exact_size(64, 1);
    CHECK(BloomFilter::deserialize(blob, restored));
    CHECK(restored.bit_count() == bf.bit_count());
    CHECK(restored.hash_count() == bf.hash_count());
    for (std::size_t i = 0; i < 1000; ++i) {
        CHECK(restored.possibly_contains("ad_imp_" + std::to_string(i)));
    }
    CHECK(!BloomFilter::deserialize("BAD!", restored));
    CHECK(!BloomFilter::deserialize(blob.substr(0, blob.size() - 1), restored));

    // Merge compatible filters vs reject incompatible
    BloomFilter a = BloomFilter::with_exact_size(4096, 5);
    BloomFilter b = BloomFilter::with_exact_size(4096, 5);
    BloomFilter c = BloomFilter::with_exact_size(8192, 5);
    for (std::uint64_t i = 0; i < 200; ++i) a.add_u64(i);
    for (std::uint64_t i = 200; i < 400; ++i) b.add_u64(i);
    CHECK(!a.merge_from(c));
    CHECK(a.merge_from(b));
    for (std::uint64_t i = 0; i < 400; ++i) {
        CHECK(a.possibly_contains_u64(i));
    }
}

// ---------------------------------------------------------------------------
// 4. InvertedIndex (galloping AND, k-way OR, AND-NOT, differential vs std::set)
// ---------------------------------------------------------------------------

void test_inverted_index() {
    // Bulk build guard + finalize dedup/sort
    {
        InvertedIndex<std::string, std::uint64_t> idx;
        idx.add_unsorted("os=ios", 50);
        idx.add_unsorted("os=ios", 10);
        idx.add_unsorted("os=ios", 50);
        CHECK(!idx.finalized());

        bool threw = false;
        try {
            (void)idx.query_and({"os=ios"});
        } catch (const std::logic_error&) {
            threw = true;
        }
        CHECK(threw);

        idx.finalize();
        CHECK(idx.finalized());
        const InvertedIndex<std::string, std::uint64_t>::PostingList* pl =
            idx.postings("os=ios");
        CHECK(pl != nullptr && pl->size() == 2 && (*pl)[0] == 10 && (*pl)[1] == 50);

        // Incremental add/remove
        CHECK(idx.add("os=ios", 30));
        CHECK(!idx.add("os=ios", 30));
        CHECK(idx.remove("os=ios", 10));
        CHECK(!idx.remove("os=ios", 10));
        pl = idx.postings("os=ios");
        CHECK(pl != nullptr && pl->size() == 2 && (*pl)[0] == 30 && (*pl)[1] == 50);
    }

    // Skewed galloping intersection (10 vs 5,000) & differential test vs std::set
    {
        InvertedIndex<std::string, std::uint64_t> idx;
        std::map<std::string, std::set<std::uint64_t> > oracle;

        // Highly skewed common term (5000 docs) vs rare term (15 docs)
        for (std::uint64_t d = 0; d < 5000; ++d) {
            idx.add_unsorted("common", d);
            oracle["common"].insert(d);
        }
        for (std::uint64_t d = 0; d < 5000; d += 333) {
            idx.add_unsorted("rare", d);
            oracle["rare"].insert(d);
        }

        // 18 randomized targeting tags across 2,000 ad IDs
        std::mt19937 rng(12345);
        std::vector<std::string> terms;
        terms.push_back("common");
        terms.push_back("rare");
        for (int t = 0; t < 18; ++t) {
            std::string name = "tag_" + std::to_string(t);
            terms.push_back(name);
            const std::size_t count = 20 + (rng() % 600);
            for (std::size_t i = 0; i < count; ++i) {
                const std::uint64_t doc = rng() % 2000;
                idx.add_unsorted(name, doc);
                oracle[name].insert(doc);
            }
        }
        idx.finalize();

        // Direct galloping AND check
        std::vector<std::uint64_t> skewed = idx.query_and({"common", "rare"});
        std::vector<std::uint64_t> expected_skewed(
            oracle["rare"].begin(), oracle["rare"].end());
        CHECK(skewed == expected_skewed);

        // 60 randomized AND / OR / AND-NOT queries checked against std::set oracle
        for (int q = 0; q < 60; ++q) {
            std::vector<std::string> pos_terms;
            std::vector<std::string> neg_terms;
            const int n_pos = 1 + static_cast<int>(rng() % 4);
            const int n_neg = static_cast<int>(rng() % 3);
            for (int i = 0; i < n_pos; ++i) {
                pos_terms.push_back(terms[rng() % terms.size()]);
            }
            for (int i = 0; i < n_neg; ++i) {
                neg_terms.push_back(terms[rng() % terms.size()]);
            }

            // AND + NOT
            std::vector<std::uint64_t> got_and = idx.query_and(pos_terms, neg_terms);
            std::set<std::uint64_t> exp_and = oracle[pos_terms[0]];
            for (std::size_t i = 1; i < pos_terms.size(); ++i) {
                std::set<std::uint64_t> next;
                const std::set<std::uint64_t>& rhs = oracle[pos_terms[i]];
                for (std::set<std::uint64_t>::const_iterator it = exp_and.begin();
                     it != exp_and.end(); ++it) {
                    if (rhs.count(*it) > 0) next.insert(*it);
                }
                exp_and.swap(next);
            }
            for (std::size_t i = 0; i < neg_terms.size(); ++i) {
                const std::set<std::uint64_t>& ex = oracle[neg_terms[i]];
                for (std::set<std::uint64_t>::const_iterator it = ex.begin();
                     it != ex.end(); ++it) {
                    exp_and.erase(*it);
                }
            }
            CHECK(got_and == std::vector<std::uint64_t>(exp_and.begin(), exp_and.end()));

            // OR + NOT (tests 1-way, 2-way, and k-way min-heap merge)
            std::vector<std::uint64_t> got_or = idx.query_or(pos_terms, neg_terms);
            std::set<std::uint64_t> exp_or;
            for (std::size_t i = 0; i < pos_terms.size(); ++i) {
                exp_or.insert(oracle[pos_terms[i]].begin(), oracle[pos_terms[i]].end());
            }
            for (std::size_t i = 0; i < neg_terms.size(); ++i) {
                const std::set<std::uint64_t>& ex = oracle[neg_terms[i]];
                for (std::set<std::uint64_t>::const_iterator it = ex.begin();
                     it != ex.end(); ++it) {
                    exp_or.erase(*it);
                }
            }
            CHECK(got_or == std::vector<std::uint64_t>(exp_or.begin(), exp_or.end()));
        }

        // Missing term in AND short-circuits cleanly
        CHECK(idx.query_and({"common", "nonexistent_term"}).empty());
    }
}

// ---------------------------------------------------------------------------
// 5. TopK (differential test vs std::sort, custom eCPM struct, min-K)
// ---------------------------------------------------------------------------

void test_top_k() {
    // k = 0 boundary
    {
        TopK<int> tk(0);
        CHECK(!tk.push(10));
        CHECK(tk.empty() && tk.full() && tk.extract_sorted().empty());
    }

    // Differential test: 10,000 random ints, TopK(50) largest & smallest
    {
        std::mt19937 rng(999);
        std::vector<int> all(10000);
        TopK<int> top_max(50);
        TopK<int, std::greater<int> > top_min(50);

        for (std::size_t i = 0; i < all.size(); ++i) {
            all[i] = static_cast<int>(rng() % 50000) - 25000;
            top_max.push(all[i]);
            top_min.push(all[i]);
        }

        CHECK(top_max.size() == 50 && top_max.full());
        std::vector<int> snap = top_max.sorted_copy();
        std::vector<int> got_max = top_max.extract_sorted();
        CHECK(snap == got_max);
        CHECK(top_max.empty());

        std::vector<int> got_min = top_min.extract_sorted();

        std::sort(all.begin(), all.end());
        std::vector<int> exp_min(all.begin(), all.begin() + 50);
        std::vector<int> exp_max(all.rbegin(), all.rbegin() + 50);
        CHECK(got_max == exp_max);
        CHECK(got_min == exp_min);
    }

    // Ad eCPM ranking struct
    {
        struct AdBid {
            std::uint64_t ad_id;
            double ecpm;
        };
        struct WorseBid {
            bool operator()(const AdBid& a, const AdBid& b) const noexcept {
                if (a.ecpm != b.ecpm) return a.ecpm < b.ecpm;
                return a.ad_id > b.ad_id; // Lower id breaks ties better
            }
        };

        TopK<AdBid, WorseBid> ranker(3);
        ranker.emplace(AdBid{10, 1.5});
        ranker.emplace(AdBid{20, 4.2});
        ranker.emplace(AdBid{30, 2.8});
        CHECK(std::fabs(ranker.worst().ecpm - 1.5) < 1e-9);
        CHECK(!ranker.emplace(AdBid{40, 1.0}));
        CHECK(ranker.emplace(AdBid{50, 3.1}));
        CHECK(std::fabs(ranker.worst().ecpm - 2.8) < 1e-9);

        std::vector<AdBid> winners = ranker.extract_sorted();
        CHECK(winners.size() == 3);
        CHECK(winners[0].ad_id == 20 && winners[1].ad_id == 50 && winners[2].ad_id == 30);
    }
}

// ---------------------------------------------------------------------------
// 6. ShardedTtlCache (deterministic FakeClock + multi-threaded stress)
// ---------------------------------------------------------------------------

void test_sharded_ttl_cache() {
    FakeClock::set_ms(1000);

    // Capacity < requested_shards caps shards cleanly so sum == exact capacity
    {
        ShardedTtlCache<std::string, int, std::hash<std::string>,
                        std::equal_to<std::string>, FakeClock>
            cache(10, 64);
        CHECK(cache.capacity() == 10);
        CHECK(cache.shard_count() == 8);
    }

    // Exact TTL boundary & purge_expired (zero sleeps!)
    {
        ShardedTtlCache<std::string, int, std::hash<std::string>,
                        std::equal_to<std::string>, FakeClock>
            cache(100, 16);

        CHECK(cache.put("short", 1, 500));  // expires at 1500
        CHECK(cache.put("long", 2, 2000));  // expires at 3000
        CHECK(cache.put("forever", 3, 0));  // never expires

        int out = 0;
        FakeClock::set_ms(1499);
        CHECK(cache.get("short", out) && out == 1);

        FakeClock::set_ms(1500);
        CHECK(!cache.get("short", out)); // Lazy eviction at exact boundary!
        CHECK(cache.get("long", out) && out == 2);
        CHECK(cache.visit("forever", [](int& v) { v += 10; }));
        CHECK(cache.get("forever", out) && out == 13);

        // Put two more expiring items and sweep via purge_expired
        CHECK(cache.put("e1", 10, 100)); // expires at 1600
        CHECK(cache.put("e2", 20, 200)); // expires at 1700
        FakeClock::set_ms(3000);
        const std::size_t purged = cache.purge_expired();
        CHECK(purged == 3); // long, e1, e2
        CHECK(cache.size() == 1);
        CHECK(cache.get("forever", out) && out == 13);
    }

    // Multi-threaded stress under real steady_clock (TSan & ASan verification)
    {
        ShardedTtlCache<int, int> cache(2048, 32);
        std::vector<std::thread> workers;
        for (int t = 0; t < 6; ++t) {
            workers.emplace_back([&, t]() {
                for (int i = 0; i < 4000; ++i) {
                    const int key = (t * 17 + i) % 512;
                    cache.put(key, i, (i % 5 == 0) ? 1 : 60000);
                    int v = 0;
                    (void)cache.get(key, v);
                    if (i % 500 == 0) {
                        (void)cache.purge_expired();
                    }
                }
            });
        }
        for (std::size_t i = 0; i < workers.size(); ++i) workers[i].join();
        CHECK(cache.size() <= cache.capacity());
    }
}

// ---------------------------------------------------------------------------
// 7. ConsistentHash (balance, weights, minimal disruption, distinct replicas)
// ---------------------------------------------------------------------------

void test_consistent_hash() {
    ConsistentHash<std::string> ring(160);
    std::string node;
    CHECK(ring.empty() && !ring.get_node("k", node));
    CHECK(ring.get_nodes("k", 3).empty());

    CHECK(ring.add_node("shard-0", 1));
    CHECK(ring.add_node("shard-1", 1));
    CHECK(ring.add_node("shard-2", 1));
    CHECK(ring.add_node("shard-3", 1));
    CHECK(ring.node_count() == 4 && ring.vnode_count() == 640);

    // Balance across 20,000 keys (expect ~25% each, assert within [18%, 32%])
    const std::size_t kKeys = 20000;
    std::map<std::string, std::size_t> counts;
    std::vector<std::string> before(kKeys);
    for (std::size_t i = 0; i < kKeys; ++i) {
        const std::string key = "query_key_" + std::to_string(i);
        CHECK(ring.get_node(key, node));
        before[i] = node;
        ++counts[node];
    }
    for (std::map<std::string, std::size_t>::const_iterator it = counts.begin();
         it != counts.end(); ++it) {
        const double frac = static_cast<double>(it->second) / static_cast<double>(kKeys);
        CHECK(frac > 0.18 && frac < 0.32);
    }

    // Distinct replica selection
    for (int i = 0; i < 200; ++i) {
        std::vector<std::string> reps = ring.get_nodes("doc_" + std::to_string(i), 3);
        CHECK(reps.size() == 3);
        std::set<std::string> uniq(reps.begin(), reps.end());
        CHECK(uniq.size() == 3);
        CHECK(ring.get_nodes("doc_" + std::to_string(i), 10).size() == 4);
    }

    // Adding a 5th node should remap ~20% of keys (verify < 32%)
    CHECK(ring.add_node("shard-4", 1));
    std::size_t moved = 0;
    for (std::size_t i = 0; i < kKeys; ++i) {
        CHECK(ring.get_node("query_key_" + std::to_string(i), node));
        if (node != before[i]) {
            ++moved;
        }
    }
    const double moved_frac = static_cast<double>(moved) / static_cast<double>(kKeys);
    CHECK(moved_frac > 0.12 && moved_frac < 0.32);

    // Weighted ring: weight 3 vs weight 1 -> ~75% vs ~25%
    ConsistentHash<std::string> weighted(160);
    CHECK(weighted.add_node("heavy", 3));
    CHECK(weighted.add_node("light", 1));
    std::size_t heavy_hits = 0;
    for (std::size_t i = 0; i < 10000; ++i) {
        CHECK(weighted.get_node("w_" + std::to_string(i), node));
        if (node == "heavy") ++heavy_hits;
    }
    const double heavy_frac = static_cast<double>(heavy_hits) / 10000.0;
    CHECK(heavy_frac > 0.65 && heavy_frac < 0.85);
}

// ---------------------------------------------------------------------------
// 8. TokenBucket & ShardedRateLimiter (deterministic FakeClock + concurrency)
// ---------------------------------------------------------------------------

void test_token_bucket() {
    FakeClock::set_ms(10000);
    {
        TokenBucket<FakeClock> tb(10.0, 5.0); // 10/s, burst 5
        for (int i = 0; i < 5; ++i) {
            CHECK(tb.try_acquire(1.0));
        }
        CHECK(!tb.try_acquire(1.0));

        // Advance 250ms -> +2.5 tokens
        FakeClock::advance_ms(250);
        CHECK(tb.try_acquire(1.0));
        CHECK(tb.try_acquire(1.0));
        CHECK(!tb.try_acquire(1.0));
        CHECK(tb.try_acquire(0.5));
        CHECK(!tb.try_acquire(0.1));

        // Advance 10s -> capped at burst 5
        FakeClock::advance_ms(10000);
        CHECK(std::fabs(tb.available_tokens() - 5.0) < 1e-6);

        // Live pacing update shrinks burst to 2
        tb.update_rate(20.0, 2.0);
        CHECK(std::fabs(tb.available_tokens() - 2.0) < 1e-6);
        CHECK(!tb.try_acquire(3.0)); // exceeds burst
        CHECK(tb.try_acquire(2.0));
    }

    // ShardedRateLimiter: bounded LRU memory + exact multi-threaded burst accounting
    {
        FakeClock::set_ms(50000);
        ShardedRateLimiter<std::string, std::hash<std::string>,
                           std::equal_to<std::string>, FakeClock>
            limiter(10.0, 100.0, 64, 8);

        // 8 threads racing on frozen clock with burst=100 -> EXACTLY 100 grants!
        std::atomic<int> granted(0);
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&]() {
                for (int i = 0; i < 50; ++i) {
                    if (limiter.try_acquire("hot_campaign", 1.0)) {
                        granted.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (std::size_t i = 0; i < threads.size(); ++i) threads[i].join();
        CHECK(granted.load() == 100);

        // High-cardinality IP flood must stay strictly bounded <= max_keys
        for (int i = 0; i < 500; ++i) {
            CHECK(limiter.try_acquire("ip_" + std::to_string(i), 1.0));
        }
        CHECK(limiter.tracked_keys() <= limiter.max_keys());
    }
}

// ---------------------------------------------------------------------------
// 9. DoubleBuffer (hot-reload snapshot + high-concurrency TSan/ASan torture)
// ---------------------------------------------------------------------------

struct TrackedSnapshot {
    static std::atomic<int> live;
    std::uint64_t generation;
    std::vector<std::uint64_t> data;
    AhoCorasick ac;

    explicit TrackedSnapshot(std::uint64_t gen) : generation(gen), data(32, gen), ac() {
        ac.add_pattern("ad_" + std::to_string(gen), gen);
        ac.add_pattern("common_kw", 999);
        ac.build();
        live.fetch_add(1, std::memory_order_relaxed);
    }

    TrackedSnapshot(const TrackedSnapshot& other)
        : generation(other.generation), data(other.data), ac(other.ac) {
        live.fetch_add(1, std::memory_order_relaxed);
    }

    ~TrackedSnapshot() noexcept {
        live.fetch_sub(1, std::memory_order_relaxed);
    }
};
std::atomic<int> TrackedSnapshot::live(0);

void test_double_buffer() {
    TrackedSnapshot::live.store(0, std::memory_order_relaxed);

    // Unit checks: nested guards, move guard, update_copy throw rollback
    {
        DoubleBuffer<std::vector<int> > db(std::vector<int>(3, 7));
        CHECK(db.version() == 1);
        {
            DoubleBuffer<std::vector<int> >::ReadGuard g1 = db.read();
            DoubleBuffer<std::vector<int> >::ReadGuard g2 = db.read(); // nested
            DoubleBuffer<std::vector<int> >::ReadGuard g3(std::move(g2));
            CHECK(!g2);
            CHECK(g1->size() == 3 && (*g3)[0] == 7);
        }
        bool threw = false;
        try {
            db.update_copy([](std::vector<int>& v) {
                v.push_back(99);
                throw std::runtime_error("reload parse error");
            });
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(db.version() == 1);
        CHECK(db.read()->size() == 3);

        db.update_copy([](std::vector<int>& v) { v.push_back(42); });
        CHECK(db.version() == 2);
        CHECK(db.read()->size() == 4 && (*db.read())[3] == 42);
    }

    // High-concurrency hot-reload stress: 8 readers + 2 writers swapping live AhoCorasick snapshots
    {
        DoubleBuffer<TrackedSnapshot> db(
            std::unique_ptr<TrackedSnapshot>(new TrackedSnapshot(1)));
        CHECK(TrackedSnapshot::live.load() == 1);

        std::atomic<bool> stop(false);
        std::atomic<std::uint64_t> total_reads(0);
        std::vector<std::thread> readers;

        for (int r = 0; r < 8; ++r) {
            readers.emplace_back([&]() {
                std::uint64_t local_reads = 0;
                while (!stop.load(std::memory_order_acquire) || local_reads < 2000) {
                    DoubleBuffer<TrackedSnapshot>::ReadGuard g = db.read();
                    const std::uint64_t gen = g->generation;
                    CHECK(g->data.size() == 32);
                    CHECK(g->data[0] == gen && g->data[31] == gen);
                    const std::string probe = "prefix_ad_" + std::to_string(gen) + "_common_kw";
                    CHECK(g->ac.contains_any(probe));
                    ++local_reads;
                }
                total_reads.fetch_add(local_reads, std::memory_order_relaxed);
            });
        }

        std::atomic<std::uint64_t> next_gen(2);
        std::vector<std::thread> writers;
        for (int w = 0; w < 2; ++w) {
            writers.emplace_back([&]() {
                for (int i = 0; i < 150; ++i) {
                    const std::uint64_t gen =
                        next_gen.fetch_add(1, std::memory_order_relaxed);
                    db.publish(std::unique_ptr<TrackedSnapshot>(new TrackedSnapshot(gen)));
                    if (i % 10 == 0) {
                        std::this_thread::yield();
                    }
                }
            });
        }

        for (std::size_t i = 0; i < writers.size(); ++i) writers[i].join();
        stop.store(true, std::memory_order_release);
        for (std::size_t i = 0; i < readers.size(); ++i) readers[i].join();

        CHECK(total_reads.load() >= 16000);
        CHECK(db.version() == 301);
        CHECK(TrackedSnapshot::live.load() == 1);
    }
    // Destructor must destroy the final active snapshot cleanly
    CHECK(TrackedSnapshot::live.load() == 0);
}

// ---------------------------------------------------------------------------
// 10. Regression tests: RadixTree alloc safety, AhoCorasick failure states,
//     ShardedTtlCache TTL extremes, ConsistentHash overflow/transactional/order
// ---------------------------------------------------------------------------

struct FakeNanoClock {
    typedef std::chrono::nanoseconds duration;
    typedef duration::rep rep;
    typedef duration::period period;
    typedef std::chrono::time_point<FakeNanoClock, duration> time_point;
    static const bool is_steady = true;

    static std::atomic<std::int64_t> now_ns;

    static time_point now() noexcept {
        return time_point(duration(now_ns.load(std::memory_order_acquire)));
    }
    static void set_ns(std::int64_t ns) noexcept {
        now_ns.store(ns, std::memory_order_release);
    }
};
std::atomic<std::int64_t> FakeNanoClock::now_ns(0);

std::uint64_t ref_mix64(std::uint64_t x) noexcept {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

std::uint64_t ref_fnv1a_mix64(const std::string& s) noexcept {
    std::uint64_t h = 14695981039346656037ULL;
    for (std::size_t i = 0; i < s.size(); ++i) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(s[i]));
        h *= 1099511628211ULL;
    }
    return ref_mix64(h ^ static_cast<std::uint64_t>(s.size()));
}

struct ConstantNodeHash {
    std::size_t operator()(const std::string&) const noexcept { return 42; }
};

struct PartialCollisionNodeHash {
    std::size_t operator()(const std::string& s) const noexcept {
        if (s == "alpha" || s == "beta") {
            return 777;
        }
        return static_cast<std::size_t>(ref_fnv1a_mix64(s));
    }
};

struct UnorderedNode {
    int id;
    int hash_bucket;
};

struct UnorderedNodeHash {
    std::size_t operator()(const UnorderedNode& n) const noexcept {
        return static_cast<std::size_t>(n.hash_bucket);
    }
};

struct UnorderedNodeEq {
    bool operator()(const UnorderedNode& a, const UnorderedNode& b) const noexcept {
        return a.id == b.id;
    }
};

void verify_radix_exact(const RadixTree<int>& tree,
                        const std::vector<std::pair<std::string, int> >& expected) {
    CHECK(tree.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const int* v = tree.find(expected[i].first);
        CHECK(v != nullptr && *v == expected[i].second);
    }
    std::vector<std::pair<std::string, int> > scanned;
    tree.prefix_scan("", [&scanned](const std::string& k, const int& v) {
        scanned.push_back(std::make_pair(k, v));
    });
    CHECK(scanned == expected);
}

void test_radix_alloc_safety() {
    // 1) Zero-allocation noexcept move constructor, move assignment, and reusable moved-from object
    {
        const std::string kEmpty;
        const std::string kApple = "apple";
        const std::string kApplication = "application";
        const std::string kApply = "apply";
        const std::string kBanana = "banana";
        const std::string kOldOne = "old_entry_one_01234567890123456789";
        const std::string kOldTwo = "old_entry_two_01234567890123456789";

        RadixTree<int> t1;
        CHECK(t1.insert(kEmpty, 0));
        CHECK(t1.insert(kApple, 1));
        CHECK(t1.insert(kApplication, 2));
        CHECK(t1.insert(kApply, 3));
        CHECK(t1.insert(kBanana, 4));

        RadixTree<int> t4;
        CHECK(t4.insert(kOldOne, 10));
        CHECK(t4.insert(kOldTwo, 20));

        RadixTree<int> t3;
        {
            AllocFaultScope no_alloc(0);
            RadixTree<int> t2(std::move(t1));
            CHECK(t1.empty() && t1.size() == 0);
            CHECK(t1.find(kEmpty) == nullptr && t1.find(kApple) == nullptr);
            CHECK(t1.longest_prefix_match(kApplication) == nullptr);
            t1.clear();

            CHECK(t2.size() == 5 && *t2.find(kApplication) == 2 && *t2.find(kEmpty) == 0);

            t3 = std::move(t2);
            CHECK(t2.empty() && t2.size() == 0 && t2.find(kApple) == nullptr);
            CHECK(t3.size() == 5 && *t3.find(kBanana) == 4);

            // Move-assign over non-empty tree tears down existing nodes with zero allocations
            t4 = std::move(t3);
            CHECK(t3.empty() && t4.size() == 5);
            CHECK(t4.find(kOldOne) == nullptr);
            CHECK(*t4.find(kApply) == 3);
            CHECK(no_alloc.calls() == 0);
        }

        // Reuse moved-from trees t1 (move-constructed from) and t3 (move-assigned from)
        CHECK(t1.insert("reused_key", 77));
        CHECK(t1.size() == 1 && *t1.find("reused_key") == 77);
        CHECK(t1.erase("reused_key") && t1.empty());

        CHECK(t3.insert("another_key", 88));
        CHECK(t3.size() == 1 && *t3.find("another_key") == 88);
    }

    // 2) Zero-allocation iterative clear() and ~RadixTree() on deep (5000-level) and wide trees
    {
        RadixTree<int> deep_for_clear;
        std::string chain;
        chain.reserve(5000);
        for (int i = 1; i <= 5000; ++i) {
            chain.push_back(static_cast<char>('a' + (i % 2)));
            CHECK(deep_for_clear.insert(chain, i));
        }
        CHECK(deep_for_clear.size() == 5000);

        {
            AllocFaultScope no_alloc(0);
            deep_for_clear.clear();
            CHECK(deep_for_clear.empty() && deep_for_clear.size() == 0);
            CHECK(no_alloc.calls() == 0);
        }

        std::unique_ptr<RadixTree<int> > deep_for_dtor(new RadixTree<int>());
        std::string chain2;
        chain2.reserve(5000);
        for (int i = 1; i <= 5000; ++i) {
            chain2.push_back('z');
            CHECK(deep_for_dtor->insert(chain2, i));
        }
        for (int b = 0; b < 128; ++b) {
            std::string wide_key(1, static_cast<char>(b + 1));
            wide_key.append(36, 'w');
            CHECK(deep_for_dtor->insert(wide_key, b));
        }
        {
            AllocFaultScope no_alloc(0);
            deep_for_dtor.reset(); // Exercises ~RadixTree() iteratively
            CHECK(no_alloc.calls() == 0);
        }
    }

    // 3) Zero-allocation SuffixMatcher::find / contains / longest_suffix_match on long keys
    {
        SuffixMatcher<int> sm;
        const std::string long_suf1 = ".very-long-suffix-domain-alpha-0123456789.example.com";
        const std::string long_suf2 = ".example.com";
        CHECK(sm.insert(long_suf1, 101));
        CHECK(sm.insert(long_suf2, 102));

        const std::string probe = "host01.sub" + long_suf1;
        const std::string miss = "completely-unrelated-very-long-hostname-0123456789.org";
        {
            AllocFaultScope no_alloc(0);
            std::size_t mlen = 0;
            const int* r1 = sm.longest_suffix_match(probe, &mlen);
            CHECK(r1 != nullptr && *r1 == 101 && mlen == long_suf1.size());
            CHECK(sm.find(long_suf1) != nullptr && *sm.find(long_suf1) == 101);
            CHECK(sm.contains(long_suf2));
            CHECK(sm.longest_suffix_match(miss) == nullptr);
            CHECK(!sm.contains(miss));
            CHECK(no_alloc.calls() == 0);
        }
    }

    // 4) Deterministic allocation-failure sweep on insert split (common < rem: diverging branch)
    {
        const std::string pfx(48, 'a');
        const std::string k_exist1 = pfx + std::string(48, 'b');
        const std::string k_exist2 = k_exist1 + "_descendant_012345678901234567890123";
        const std::string k_new = pfx + std::string(48, 'c');

        std::vector<std::pair<std::string, int> > baseline;
        baseline.push_back(std::make_pair(k_exist1, 10));
        baseline.push_back(std::make_pair(k_exist2, 20));

        std::size_t failures = 0;
        bool succeeded = false;
        for (std::size_t fail_at = 0; fail_at < 64; ++fail_at) {
            RadixTree<int> tree;
            CHECK(tree.insert(k_exist1, 10));
            CHECK(tree.insert(k_exist2, 20));

            bool threw = false;
            try {
                AllocFaultScope fault(fail_at);
                CHECK(tree.insert(k_new, 30));
            } catch (const std::bad_alloc&) {
                threw = true;
            }

            if (!threw) {
                CHECK(tree.size() == 3);
                CHECK(*tree.find(k_exist1) == 10);
                CHECK(*tree.find(k_exist2) == 20);
                CHECK(*tree.find(k_new) == 30);
                succeeded = true;
                break;
            }
            ++failures;
            CHECK(tree.find(k_new) == nullptr);
            verify_radix_exact(tree, baseline);

            // Recovery: subsequent insert succeeds cleanly without corruption
            CHECK(tree.insert(k_new, 30));
            CHECK(tree.size() == 3 && *tree.find(k_new) == 30);
        }
        CHECK(succeeded && failures >= 3);
    }

    // 5) Deterministic allocation-failure sweep on insert split (common == rem: prefix key)
    {
        const std::string pfx(48, 'p');
        const std::string k_exist = pfx + std::string(48, 'q');

        std::vector<std::pair<std::string, int> > baseline;
        baseline.push_back(std::make_pair(k_exist, 11));

        std::size_t failures = 0;
        bool succeeded = false;
        for (std::size_t fail_at = 0; fail_at < 64; ++fail_at) {
            RadixTree<int> tree;
            CHECK(tree.insert(k_exist, 11));

            bool threw = false;
            try {
                AllocFaultScope fault(fail_at);
                CHECK(tree.insert(pfx, 22));
            } catch (const std::bad_alloc&) {
                threw = true;
            }

            if (!threw) {
                CHECK(tree.size() == 2 && *tree.find(pfx) == 22 && *tree.find(k_exist) == 11);
                succeeded = true;
                break;
            }
            ++failures;
            CHECK(tree.find(pfx) == nullptr);
            verify_radix_exact(tree, baseline);

            CHECK(tree.insert(pfx, 22));
            CHECK(tree.size() == 2 && *tree.find(pfx) == 22 && *tree.find(k_exist) == 11);
        }
        CHECK(succeeded && failures >= 2);
    }

    // 6) Deterministic allocation-failure sweep on erase coalescing (self-coalesce: cur has 1 child)
    {
        const std::string k1(48, 'm');
        const std::string k2 = k1 + std::string(48, 'n');
        std::vector<std::pair<std::string, int> > baseline;
        baseline.push_back(std::make_pair(k1, 1));
        baseline.push_back(std::make_pair(k2, 2));

        std::size_t failures = 0;
        bool succeeded = false;
        for (std::size_t fail_at = 0; fail_at < 64; ++fail_at) {
            RadixTree<int> tree;
            CHECK(tree.insert(k1, 1));
            CHECK(tree.insert(k2, 2));

            bool threw = false;
            try {
                AllocFaultScope fault(fail_at);
                CHECK(tree.erase(k1));
            } catch (const std::bad_alloc&) {
                threw = true;
            }

            if (!threw) {
                CHECK(tree.size() == 1 && tree.find(k1) == nullptr && *tree.find(k2) == 2);
                succeeded = true;
                break;
            }
            ++failures;
            verify_radix_exact(tree, baseline);

            // Recovery after allocation failure
            CHECK(tree.erase(k1));
            CHECK(tree.size() == 1 && tree.find(k1) == nullptr && *tree.find(k2) == 2);
        }
        CHECK(succeeded && failures >= 2);
    }

    // 7) Deterministic allocation-failure sweep on erase coalescing (parent has 2 children,
    //    erasing child 1 and erasing child 0, with multi-leaf survivor subtree)
    {
        const std::string p(48, 'r');
        const std::string s0(48, 's');
        const std::string s1(48, 't');
        const std::string k_a = p + s0 + "_leaf_a";
        const std::string k_b = p + s0 + "_leaf_b";
        const std::string k_victim = p + s1;

        std::vector<std::pair<std::string, int> > baseline;
        baseline.push_back(std::make_pair(k_a, 1));
        baseline.push_back(std::make_pair(k_b, 2));
        baseline.push_back(std::make_pair(k_victim, 3));

        std::size_t failures = 0;
        bool succeeded = false;
        for (std::size_t fail_at = 0; fail_at < 64; ++fail_at) {
            RadixTree<int> tree;
            CHECK(tree.insert(k_a, 1));
            CHECK(tree.insert(k_b, 2));
            CHECK(tree.insert(k_victim, 3));

            bool threw = false;
            try {
                AllocFaultScope fault(fail_at);
                CHECK(tree.erase(k_victim));
            } catch (const std::bad_alloc&) {
                threw = true;
            }

            if (!threw) {
                CHECK(tree.size() == 2 && tree.find(k_victim) == nullptr);
                CHECK(*tree.find(k_a) == 1 && *tree.find(k_b) == 2);
                succeeded = true;
                break;
            }
            ++failures;
            verify_radix_exact(tree, baseline);

            CHECK(tree.erase(k_victim));
            CHECK(tree.size() == 2 && *tree.find(k_a) == 1 && *tree.find(k_b) == 2);
        }
        CHECK(succeeded && failures >= 2);

        // Also test erasing child index 0 (smaller byte) while child index 1 survives
        const std::string k_first = p + std::string(48, 'a');
        const std::string k_second = p + std::string(48, 'z');
        std::vector<std::pair<std::string, int> > pair_base;
        pair_base.push_back(std::make_pair(k_first, 10));
        pair_base.push_back(std::make_pair(k_second, 20));

        failures = 0;
        succeeded = false;
        for (std::size_t fail_at = 0; fail_at < 64; ++fail_at) {
            RadixTree<int> tree;
            CHECK(tree.insert(k_first, 10));
            CHECK(tree.insert(k_second, 20));

            bool threw = false;
            try {
                AllocFaultScope fault(fail_at);
                CHECK(tree.erase(k_first));
            } catch (const std::bad_alloc&) {
                threw = true;
            }

            if (!threw) {
                CHECK(tree.size() == 1 && tree.find(k_first) == nullptr && *tree.find(k_second) == 20);
                succeeded = true;
                break;
            }
            ++failures;
            verify_radix_exact(tree, pair_base);
        }
        CHECK(succeeded && failures >= 2);
    }
}

void test_aho_corasick_failure_states() {
    // 1) Failed add_pattern (new branch) on an already-built automaton must invalidate
    //    built_, roll back partial trie/pattern changes, reject scans, and recover via build().
    {
        const std::string long_new_pat = "unique_pattern_0123456789_abcdefghijklmnopqrstuvwxyz";
        std::size_t failures = 0;
        bool succeeded = false;

        for (std::size_t fail_at = 0; fail_at < 128; ++fail_at) {
            AhoCorasick ac;
            ac.add_pattern("he", 10);
            ac.add_pattern("she", 20);
            ac.add_pattern("hers", 30);
            ac.build();
            CHECK(ac.built());
            const std::size_t orig_patterns = ac.pattern_count();
            const std::size_t orig_nodes = ac.node_count();
            const std::vector<AhoCorasick::Match> baseline = ac.find_all("ushers");
            CHECK(baseline.size() == 3);

            bool threw = false;
            try {
                AllocFaultScope fault(fail_at);
                (void)ac.add_pattern(long_new_pat, 999);
            } catch (const std::bad_alloc&) {
                threw = true;
            }

            if (!threw) {
                CHECK(!ac.built());
                CHECK(ac.pattern_count() == orig_patterns + 1);
                ac.build();
                CHECK(ac.built());
                CHECK(ac.contains_any("xx_" + long_new_pat + "_yy"));
                CHECK(ac.find_all("ushers").size() == 3);
                succeeded = true;
                break;
            }

            ++failures;
            CHECK(!ac.built());
            CHECK(ac.pattern_count() == orig_patterns);
            CHECK(ac.node_count() == orig_nodes);

            bool scan_threw = false;
            try {
                (void)ac.contains_any("ushers");
            } catch (const std::logic_error&) {
                scan_threw = true;
            }
            CHECK(scan_threw);

            // Recovery: build() succeeds and restores exact prior matches with zero corruption
            ac.build();
            CHECK(ac.built());
            const std::vector<AhoCorasick::Match> recovered = ac.find_all("ushers");
            CHECK(recovered.size() == baseline.size());
            for (std::size_t i = 0; i < baseline.size(); ++i) {
                CHECK(recovered[i].pattern_id == baseline[i].pattern_id);
                CHECK(recovered[i].tag == baseline[i].tag);
                CHECK(recovered[i].start == baseline[i].start);
                CHECK(recovered[i].length == baseline[i].length);
            }
            CHECK(!ac.contains_any(long_new_pat));
        }
        CHECK(succeeded && failures >= 2);
    }

    // 2) Failed add_pattern on an existing terminal node (new_nodes_needed == 0)
    {
        std::size_t failures = 0;
        bool succeeded = false;
        for (std::size_t fail_at = 0; fail_at < 32; ++fail_at) {
            AhoCorasick ac;
            ac.add_pattern("he", 10);
            ac.build();
            const std::size_t orig_patterns = ac.pattern_count();
            const std::size_t orig_nodes = ac.node_count();

            bool threw = false;
            try {
                AllocFaultScope fault(fail_at);
                // outputs has size 1, capacity 1 (and patterns_ has size 1, capacity 1),
                // so adding a duplicate pattern deterministically triggers reallocation.
                (void)ac.add_pattern("he", 20);
            } catch (const std::bad_alloc&) {
                threw = true;
            }

            if (!threw) {
                CHECK(!ac.built() && ac.pattern_count() == orig_patterns + 1);
                ac.build();
                CHECK(ac.find_all("he").size() == 2);
                succeeded = true;
                break;
            }

            ++failures;
            CHECK(!ac.built());
            CHECK(ac.pattern_count() == orig_patterns);
            CHECK(ac.node_count() == orig_nodes);
            bool scan_threw = false;
            try {
                (void)ac.find_all("he");
            } catch (const std::logic_error&) {
                scan_threw = true;
            }
            CHECK(scan_threw);

            ac.build();
            CHECK(ac.built());
            CHECK(ac.find_all("he").size() == 1);
        }
        CHECK(succeeded && failures >= 1);
    }

    // 3) Failed build() on both already-built and unbuilt automatons must leave built() == false
    //    and recover cleanly once allocations succeed again.
    {
        AhoCorasick ac;
        ac.add_pattern("he", 1);
        ac.add_pattern("she", 2);
        ac.add_pattern("hers", 3);
        ac.build();
        CHECK(ac.built());

        bool threw = false;
        try {
            AllocFaultScope fault(0);
            ac.build();
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(!ac.built());

        bool scan_threw = false;
        try {
            (void)ac.contains_any("ushers");
        } catch (const std::logic_error&) {
            scan_threw = true;
        }
        CHECK(scan_threw);

        // Recover via subsequent build()
        ac.build();
        CHECK(ac.built());
        CHECK(ac.find_all("ushers").size() == 3);

        // Also fail build() right after adding a new pattern (unbuilt state)
        ac.add_pattern("us", 4);
        CHECK(!ac.built());
        threw = false;
        try {
            AllocFaultScope fault(0);
            ac.build();
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        CHECK(threw && !ac.built());
        ac.build();
        CHECK(ac.built() && ac.find_all("ushers").size() == 4);
    }
}

void test_sharded_ttl_cache_extremes() {
    // 1) Real steady_clock: max uint64, INT64_MAX, INT64_MAX+1 must not wrap/expire immediately
    {
        ShardedTtlCache<int, std::string> cache(8, 4);
        const std::uint64_t u64_max = std::numeric_limits<std::uint64_t>::max();
        const std::uint64_t i64_max =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

        CHECK(cache.put(1, std::string("u64max"), u64_max));
        std::string lval = "i64max";
        CHECK(cache.put(2, lval, i64_max));
        CHECK(cache.put(3, std::string("i64max_plus1"), i64_max + 1ULL));
        CHECK(cache.put(4, std::string("no_expiry"), 0));

        std::string out;
        CHECK(cache.get(1, out) && out == "u64max");
        CHECK(cache.get(2, out) && out == "i64max");
        CHECK(cache.get(3, out) && out == "i64max_plus1");
        CHECK(cache.get(4, out) && out == "no_expiry");
        CHECK(cache.purge_expired() == 0);
        CHECK(cache.get(1, out) && out == "u64max");
    }

    // 2) FakeClock (milliseconds) boundary now + ttl near INT64_MAX and negative now
    {
        const std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
        const std::uint64_t u64_max = std::numeric_limits<std::uint64_t>::max();

        ShardedTtlCache<std::string, int, std::hash<std::string>,
                        std::equal_to<std::string>, FakeClock>
            cache(32, 4);

        FakeClock::set_ms(kMax - 100);
        CHECK(cache.put("exact_99", 1, 99));        // expires at kMax - 1
        CHECK(cache.put("exact_100", 2, 100));      // expires at kMax
        CHECK(cache.put("over_101", 3, 101));       // saturates to kMax
        CHECK(cache.put("over_u64max", 4, u64_max));// saturates to kMax
        CHECK(cache.put("forever", 5, 0));          // no expiry

        int v = 0;
        FakeClock::set_ms(kMax - 2);
        CHECK(cache.get("exact_99", v) && v == 1);
        CHECK(cache.get("exact_100", v) && v == 2);
        CHECK(cache.get("over_101", v) && v == 3);
        CHECK(cache.get("over_u64max", v) && v == 4);
        CHECK(cache.get("forever", v) && v == 5);

        FakeClock::set_ms(kMax - 1);
        CHECK(!cache.get("exact_99", v));           // expired at kMax - 1
        CHECK(cache.get("exact_100", v) && v == 2); // still valid before kMax
        CHECK(cache.get("over_101", v) && v == 3);
        CHECK(cache.get("over_u64max", v) && v == 4);
        CHECK(cache.get("forever", v) && v == 5);

        FakeClock::set_ms(kMax);
        CHECK(!cache.get("exact_100", v));
        CHECK(!cache.get("over_101", v));
        CHECK(!cache.get("over_u64max", v));
        CHECK(cache.get("forever", v) && v == 5);   // no-expiry still survives at kMax!

        // Negative clock + TTL > INT64_MAX that fits in headroom vs saturating
        FakeClock::set_ms(-1000);
        const std::uint64_t fit_ttl = static_cast<std::uint64_t>(kMax) + 500ULL;
        const std::uint64_t sat_ttl = static_cast<std::uint64_t>(kMax) + 1000ULL;
        CHECK(cache.put("neg_fit", 10, fit_ttl));   // exact expiry at kMax - 500
        CHECK(cache.put("neg_sat", 11, sat_ttl));   // saturates to kMax

        FakeClock::set_ms(kMax - 501);
        CHECK(cache.get("neg_fit", v) && v == 10);
        CHECK(cache.get("neg_sat", v) && v == 11);

        FakeClock::set_ms(kMax - 500);
        CHECK(!cache.get("neg_fit", v));
        CHECK(cache.get("neg_sat", v) && v == 11);

        FakeClock::set_ms(kMax - 1);
        CHECK(cache.get("neg_sat", v) && v == 11);
        FakeClock::set_ms(kMax);
        CHECK(!cache.get("neg_sat", v));
    }

    // 3) FakeNanoClock (nanoseconds, 1ms = 1,000,000 ticks) multiplication overflow boundaries
    {
        const std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
        const std::uint64_t u64_max = std::numeric_limits<std::uint64_t>::max();
        const std::uint64_t kNsPerMs = 1000000ULL;

        ShardedTtlCache<std::string, int, std::hash<std::string>,
                        std::equal_to<std::string>, FakeNanoClock>
            ncache(16, 4);

        FakeNanoClock::set_ns(kMax - 10 * static_cast<std::int64_t>(kNsPerMs));
        CHECK(ncache.put("ns_5ms", 1, 5));                        // expires at kMax - 5ms
        CHECK(ncache.put("ns_10ms", 2, 10));                      // saturates to kMax
        CHECK(ncache.put("ns_mul_edge", 3, u64_max / kNsPerMs));  // saturates to kMax
        CHECK(ncache.put("ns_mul_over", 4, u64_max / kNsPerMs + 1ULL)); // saturates to kMax
        CHECK(ncache.put("ns_u64max", 5, u64_max));               // saturates to kMax

        int v = 0;
        FakeNanoClock::set_ns(kMax - 5 * static_cast<std::int64_t>(kNsPerMs) - 1);
        CHECK(ncache.get("ns_5ms", v) && v == 1);
        FakeNanoClock::set_ns(kMax - 5 * static_cast<std::int64_t>(kNsPerMs));
        CHECK(!ncache.get("ns_5ms", v));
        CHECK(ncache.get("ns_10ms", v) && v == 2);
        CHECK(ncache.get("ns_mul_edge", v) && v == 3);
        CHECK(ncache.get("ns_mul_over", v) && v == 4);
        CHECK(ncache.get("ns_u64max", v) && v == 5);

        FakeNanoClock::set_ns(kMax - 1);
        CHECK(ncache.get("ns_10ms", v) && v == 2);
        CHECK(ncache.get("ns_mul_over", v) && v == 4);
        CHECK(ncache.get("ns_u64max", v) && v == 5);

        FakeNanoClock::set_ns(kMax);
        CHECK(!ncache.get("ns_10ms", v));
        CHECK(!ncache.get("ns_mul_edge", v));
        CHECK(!ncache.get("ns_mul_over", v));
        CHECK(!ncache.get("ns_u64max", v));
    }
}

void test_consistent_hash_hardening() {
    // 1) Overflow validation before mutation (empty ring and existing-node update)
    {
        ConsistentHash<> ring(2);
        const std::size_t overflow_weight =
            std::numeric_limits<std::size_t>::max() / 2 + 1;

        bool threw = false;
        try {
            ring.add_node("node", overflow_weight);
        } catch (const std::overflow_error&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(ring.empty() && ring.node_count() == 0 && ring.vnode_count() == 0);

        CHECK(ring.add_node("node", 1));
        CHECK(ring.node_count() == 1 && ring.vnode_count() == 2);

        // Updating existing node with overflow weight must NOT remove existing node
        threw = false;
        try {
            ring.add_node("node", overflow_weight);
        } catch (const std::overflow_error&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(ring.node_count() == 1 && ring.vnode_count() == 2 && ring.contains_node("node"));
        std::string picked;
        CHECK(ring.get_node("any-key", picked) && picked == "node");

        // Zero weight on existing node must reject without removing existing node
        threw = false;
        try {
            ring.add_node("node", 0);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(ring.node_count() == 1 && ring.vnode_count() == 2 && ring.contains_node("node"));
    }

    // 2) Transactional add_node under deterministic allocation failures (new node + weight upgrade)
    {
        // New node insertion rollback: no orphan ring points left behind
        std::size_t failures = 0;
        bool succeeded = false;
        for (std::size_t fail_at = 0; fail_at < 64; ++fail_at) {
            ConsistentHash<> ring(8);
            CHECK(ring.add_node("n1", 1));
            CHECK(ring.node_count() == 1 && ring.vnode_count() == 8);

            bool threw = false;
            try {
                AllocFaultScope fault(fail_at);
                CHECK(ring.add_node("n2", 2));
            } catch (const std::bad_alloc&) {
                threw = true;
            }

            if (!threw) {
                CHECK(ring.node_count() == 2 && ring.vnode_count() == 24);
                CHECK(ring.contains_node("n1") && ring.contains_node("n2"));
                succeeded = true;
                break;
            }

            ++failures;
            CHECK(ring.node_count() == 1 && ring.vnode_count() == 8);
            CHECK(ring.contains_node("n1") && !ring.contains_node("n2"));
            for (int k = 0; k < 20; ++k) {
                std::string out;
                CHECK(ring.get_node("probe_" + std::to_string(k), out) && out == "n1");
            }
        }
        CHECK(succeeded && failures >= 2);

        // Existing node weight upgrade rollback: old node & exact vnode count preserved
        failures = 0;
        succeeded = false;
        for (std::size_t fail_at = 0; fail_at < 64; ++fail_at) {
            ConsistentHash<> ring(8);
            CHECK(ring.add_node("n1", 1));
            CHECK(ring.add_node("n2", 1));
            std::vector<std::string> baseline_route(40);
            for (int k = 0; k < 40; ++k) {
                CHECK(ring.get_node("rk_" + std::to_string(k), baseline_route[static_cast<std::size_t>(k)]));
            }

            bool threw = false;
            try {
                AllocFaultScope fault(fail_at);
                CHECK(!ring.add_node("n1", 4));
            } catch (const std::bad_alloc&) {
                threw = true;
            }

            if (!threw) {
                CHECK(ring.node_count() == 2 && ring.vnode_count() == 40);
                // Shrink back to weight 1 also works cleanly
                CHECK(!ring.add_node("n1", 1));
                CHECK(ring.node_count() == 2 && ring.vnode_count() == 16);
                for (int k = 0; k < 40; ++k) {
                    std::string out;
                    CHECK(ring.get_node("rk_" + std::to_string(k), out));
                    CHECK(out == baseline_route[static_cast<std::size_t>(k)]);
                }
                succeeded = true;
                break;
            }

            ++failures;
            CHECK(ring.node_count() == 2 && ring.vnode_count() == 16);
            CHECK(ring.contains_node("n1") && ring.contains_node("n2"));
            for (int k = 0; k < 40; ++k) {
                std::string out;
                CHECK(ring.get_node("rk_" + std::to_string(k), out));
                CHECK(out == baseline_route[static_cast<std::size_t>(k)]);
            }
        }
        CHECK(succeeded && failures >= 2);
    }

    // 3) Portable deterministic default std::string node hashing (independent of std::hash<string>)
    {
        const std::size_t kVnodes = 16;
        ConsistentHash<> ring(kVnodes);
        const std::vector<std::string> nodes = {"node-alpha", "node-beta", "node-gamma"};
        std::multimap<std::uint64_t, std::string> ref_ring;

        for (std::size_t ni = 0; ni < nodes.size(); ++ni) {
            CHECK(ring.add_node(nodes[ni], 1));
            const std::uint64_t seed = ref_fnv1a_mix64(nodes[ni]);
            for (std::size_t vi = 0; vi < kVnodes; ++vi) {
                const std::uint64_t pt = ref_mix64(
                    seed ^ ref_mix64(static_cast<std::uint64_t>(vi) + 0x517cc1b727220a95ULL));
                ref_ring.emplace(pt, nodes[ni]);
            }
        }

        for (int k = 0; k < 80; ++k) {
            const std::string key = "portable_key_" + std::to_string(k);
            const std::uint64_t kh = ref_fnv1a_mix64(key);
            std::multimap<std::uint64_t, std::string>::const_iterator it =
                ref_ring.lower_bound(kh);
            if (it == ref_ring.end()) {
                it = ref_ring.begin();
            }
            std::string got;
            CHECK(ring.get_node(key, got) && got == it->second);
        }
    }

    // 4) Collision & insertion-order stability (full collision, partial collision, default ring)
    {
        // Full collision via ConstantNodeHash across all 6 permutations of 3 nodes
        std::vector<std::string> perm = {"node-a", "node-b", "node-c"};
        std::sort(perm.begin(), perm.end());
        do {
            ConsistentHash<std::string, ConstantNodeHash> cring(10);
            for (std::size_t i = 0; i < perm.size(); ++i) {
                CHECK(cring.add_node(perm[i], 1));
            }
            CHECK(cring.node_count() == 3 && cring.vnode_count() == 30);

            for (int k = 0; k < 50; ++k) {
                const std::string key = "col_" + std::to_string(k);
                std::string primary;
                CHECK(cring.get_node(key, primary) && primary == "node-a");
                std::vector<std::string> reps = cring.get_nodes(key, 3);
                CHECK(reps.size() == 3);
                CHECK(reps[0] == "node-a" && reps[1] == "node-b" && reps[2] == "node-c");
            }

            // Removing "node-a" leaves "node-b" and "node-c" completely intact
            CHECK(cring.remove_node("node-a"));
            CHECK(cring.node_count() == 2 && cring.vnode_count() == 20);
            std::string primary;
            CHECK(cring.get_node("col_0", primary) && primary == "node-b");
            std::vector<std::string> reps2 = cring.get_nodes("col_0", 2);
            CHECK(reps2.size() == 2 && reps2[0] == "node-b" && reps2[1] == "node-c");

            // Re-adding "node-a" restores exact sorted tie-break
            CHECK(cring.add_node("node-a", 1));
            CHECK(cring.get_node("col_0", primary) && primary == "node-a");
        } while (std::next_permutation(perm.begin(), perm.end()));

        // Partial collision ("alpha" and "beta" collide; "gamma" and "delta" distinct)
        ConsistentHash<std::string, PartialCollisionNodeHash> p1(12);
        ConsistentHash<std::string, PartialCollisionNodeHash> p2(12);
        CHECK(p1.add_node("alpha", 1));
        CHECK(p1.add_node("beta", 1));
        CHECK(p1.add_node("gamma", 2));
        CHECK(p1.add_node("delta", 1));

        CHECK(p2.add_node("delta", 1));
        CHECK(p2.add_node("gamma", 2));
        CHECK(p2.add_node("beta", 1));
        CHECK(p2.add_node("alpha", 1));

        CHECK(p1.vnode_count() == p2.vnode_count());
        for (int k = 0; k < 200; ++k) {
            const std::string key = "pcol_" + std::to_string(k);
            std::string o1, o2;
            CHECK(p1.get_node(key, o1) && p2.get_node(key, o2) && o1 == o2);
            CHECK(p1.get_nodes(key, 4) == p2.get_nodes(key, 4));
        }

        // Default std::string ring insertion-order invariance
        ConsistentHash<> d1(64);
        ConsistentHash<> d2(64);
        const std::vector<std::string> fwd = {"s0", "s1", "s2", "s3", "s4"};
        const std::vector<std::string> rev = {"s4", "s2", "s0", "s3", "s1"};
        for (std::size_t i = 0; i < fwd.size(); ++i) {
            CHECK(d1.add_node(fwd[i], (i % 2) + 1));
            const std::size_t orig_idx = static_cast<std::size_t>(rev[i][1] - '0');
            CHECK(d2.add_node(rev[i], (orig_idx % 2) + 1));
        }
        for (int k = 0; k < 200; ++k) {
            const std::string key = "inv_" + std::to_string(k);
            std::string o1, o2;
            CHECK(d1.get_node(key, o1) && d2.get_node(key, o2) && o1 == o2);
            CHECK(d1.get_nodes(key, 3) == d2.get_nodes(key, 3));
        }

        // Custom Node type WITHOUT operator< (explicit custom Hash & KeyEqual)
        ConsistentHash<UnorderedNode, UnorderedNodeHash, UnorderedNodeEq> ur(8);
        CHECK(ur.add_node(UnorderedNode{1, 100}, 1));
        CHECK(ur.add_node(UnorderedNode{2, 100}, 1)); // collides with id=1
        CHECK(ur.add_node(UnorderedNode{3, 200}, 1));
        CHECK(ur.node_count() == 3 && ur.vnode_count() == 24);
        std::vector<UnorderedNode> u_reps = ur.get_nodes("ukey", 3);
        CHECK(u_reps.size() == 3);
        CHECK(ur.remove_node(UnorderedNode{1, 100}));
        CHECK(ur.node_count() == 2 && ur.vnode_count() == 16);
    }
}


void test_review_growth_and_remove_rollback() {
    AhoCorasick matcher;
    const std::string pattern = "duplicate-pattern";
    std::size_t allocations = 0;
    {
        AllocFaultScope count_only(std::numeric_limits<std::size_t>::max());
        for (int i = 0; i < 4096; ++i) {
            matcher.add_pattern(pattern);
        }
        allocations = count_only.calls();
    }
    CHECK(allocations < 128);
    matcher.build();
    CHECK(matcher.find_all(pattern).size() == 4096);

    struct CollisionHash {
        std::size_t operator()(const std::string&) const { return 0; }
    };
    struct ThrowingEqual {
        std::shared_ptr<int> remaining;
        bool operator()(const std::string& a, const std::string& b) const {
            if (*remaining == 0) throw std::runtime_error("injected equality failure");
            if (*remaining > 0) --*remaining;
            return a == b;
        }
    };
    int failures = 0;
    for (int fail_at = 0; fail_at < 16; ++fail_at) {
        std::shared_ptr<int> remaining(new int(-1));
        ConsistentHash<std::string, CollisionHash, ThrowingEqual> ring(
            8, CollisionHash(), ThrowingEqual{remaining});
        ring.add_node("a");
        ring.add_node("b");
        const std::vector<std::string> before = ring.get_nodes("lookup", 2);
        *remaining = fail_at;
        bool threw = false;
        try { ring.remove_node("a"); }
        catch (const std::runtime_error&) { threw = true; }
        *remaining = -1;
        if (threw) {
            ++failures;
            CHECK(ring.node_count() == 2 && ring.vnode_count() == 16);
            CHECK(ring.get_nodes("lookup", 2) == before);
            CHECK(ring.remove_node("a"));
        }
        CHECK(ring.node_count() == 1 && ring.vnode_count() == 8);
        CHECK(!ring.contains_node("a") && ring.contains_node("b"));
    }
    CHECK(failures > 1);
}

void test_claude_audit_p1_p2_regressions() {
    // 1. P1: Moved-from BloomFilter must be safe (no OOB on add/query/merge/serialize)
    {
        BloomFilter a(1000, 0.01);
        a.add("hello");
        a.add_u64(0x0102030405060708ULL);

        BloomFilter b(std::move(a));
        CHECK(b.possibly_contains("hello"));
        CHECK(b.possibly_contains_u64(0x0102030405060708ULL));
        CHECK(a.bit_count() == 0 && a.hash_count() == 0);
        // Operations on moved-from `a` must not crash/OOB under ASan/UBSan
        a.add("should_noop");
        CHECK(!a.possibly_contains("should_noop"));
        CHECK(!b.merge_from(a));
        CHECK(!a.merge_from(b));
        CHECK(a.serialize().empty());

        // Move assignment also resets source
        BloomFilter c = BloomFilter::with_exact_size(128, 2);
        c = std::move(b);
        CHECK(b.bit_count() == 0);
        CHECK(!b.possibly_contains("hello"));
        CHECK(c.possibly_contains("hello"));
    }

    // 2. P1: ShardedRateLimiter eviction hardening + relative epsilon (no 1e-13 bypass)
    {
        FakeClock::set_ms(100000);
        TokenBucket<FakeClock> tb(10.0, 5.0, 0.0);
        CHECK(!tb.try_acquire(1e-13)); // Must NOT bypass empty bucket via absolute epsilon!
        CHECK(tb.try_acquire(0.0));    // 0.0 permits allowed

        // 1 shard of capacity 4, burst 50: drain abuser, evict via spoofed keys,
        // abuser must NOT get reset to burst=50 when re-admitted under eviction pressure.
        ShardedRateLimiter<std::string, std::hash<std::string>,
                           std::equal_to<std::string>, FakeClock>
            rl(1.0, 50.0, 4, 1);
        for (int i = 0; i < 50; ++i) {
            CHECK(rl.try_acquire("abuser", 1.0));
        }
        CHECK(!rl.try_acquire("abuser", 1.0));
        // Evict "abuser" with 4 distinct spoofed keys
        for (int i = 0; i < 4; ++i) {
            CHECK(rl.try_acquire("spoof_" + std::to_string(i), 1.0));
        }
        // Under eviction pressure, multi-permit burst fails immediately
        CHECK(!rl.try_acquire("abuser", 2.0));
        // At most 1 request passes, then immediately 0 tokens (NOT 49!)
        CHECK(rl.try_acquire("abuser", 1.0));
        CHECK(!rl.try_acquire("abuser", 1.0));
    }

    // 3. P2: Moved-from AhoCorasick is safe to rebuild and reuse
    {
        AhoCorasick a;
        a.add_pattern("alpha", 1);
        a.build();
        CHECK(a.built());

        AhoCorasick b(std::move(a));
        CHECK(b.built() && b.contains_any("xxalphayy"));
        CHECK(!a.built());
        bool threw = false;
        try {
            (void)a.contains_any("alpha");
        } catch (const std::logic_error&) {
            threw = true;
        }
        CHECK(threw);
        // Reusing moved-from `a` must not OOB on nodes_[0]
        a.add_pattern("beta", 2);
        a.build();
        CHECK(a.built() && a.contains_any("beta") && !a.contains_any("alpha"));
    }

    // 4. P2: RadixTree iterative prefix_scan on deep 2000-level chain + matched_len=0 on miss
    {
        RadixTree<int> deep;
        std::string key;
        for (int i = 1; i <= 2000; ++i) {
            key.push_back('a');
            CHECK(deep.insert(key, i));
        }
        std::size_t visited = 0;
        std::size_t last_len = 0;
        deep.prefix_scan("a", [&](const std::string& k, const int& v) {
            ++visited;
            CHECK(k.size() == static_cast<std::size_t>(v));
            CHECK(k.size() > last_len);
            last_len = k.size();
        });
        CHECK(visited == 2000);

        std::size_t mlen = 777;
        CHECK(deep.longest_prefix_match("zzz", &mlen) == nullptr);
        CHECK(mlen == 0);

        SuffixMatcher<int> sm;
        sm.insert(".com", 1);
        mlen = 888;
        CHECK(sm.longest_suffix_match("nope.org", &mlen) == nullptr);
        CHECK(mlen == 0);
    }

    // 5. P2: ConsistentHash max_total_vnodes safety cap + small-count get_nodes linear scan
    {
        ConsistentHash<std::string> capped(100, myframe::containers::DefaultNodeHash<std::string>(),
                                           std::equal_to<std::string>(), 250);
        CHECK(capped.add_node("n1", 2)); // 200 vnodes <= 250
        bool threw_len = false;
        try {
            (void)capped.add_node("n2", 1); // +100 -> 300 > 250
        } catch (const std::length_error&) {
            threw_len = true;
        }
        CHECK(threw_len);
        CHECK(capped.node_count() == 1 && capped.vnode_count() == 200);
    }

    // 6. P2: ShardedTtlCache bounded purge_expired
    {
        FakeClock::set_ms(1000);
        ShardedTtlCache<int, int, std::hash<int>, std::equal_to<int>, FakeClock>
            cache(20, 1);
        for (int i = 0; i < 10; ++i) {
            CHECK(cache.put(i, i, 100)); // all expire at 1100
        }
        FakeClock::set_ms(2000);
        // Bounded sweep of 4 entries per shard removes only 4 entries (bounded lock hold)
        CHECK(cache.purge_expired(4) == 4);
        CHECK(cache.size() == 6);
        CHECK(cache.purge_expired(0) == 6);
        CHECK(cache.size() == 0);
    }

    // 7. P2/P3: DoubleBuffer ReadGuard::version() exact snapshot match + InvertedIndex dedup
    {
        DoubleBuffer<int> db(10);
        DoubleBuffer<int>::ReadGuard g1 = db.read();
        CHECK(g1.version() == 1 && *g1 == 10);
        // Move guard out while publishing from another thread
        std::thread writer([&]() { db.publish(20); });
        // Give writer a brief moment to flip before releasing g1
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(g1.version() == 1 && *g1 == 10);
        g1 = DoubleBuffer<int>::ReadGuard(); // release g1 -> unblocks writer
        writer.join();
        DoubleBuffer<int>::ReadGuard g2 = db.read();
        CHECK(g2.version() == 2 && *g2 == 20);
    }

    {
        InvertedIndex<std::string, std::uint64_t> idx;
        idx.add("a", 1);
        idx.add("a", 2);
        idx.add("b", 2);
        idx.add("b", 3);
        // Equal-size terms with duplicate "a": {a, b, a}
        std::vector<std::uint64_t> res = idx.query_and({"a", "b", "a"});
        CHECK(res.size() == 1 && res[0] == 2);
    }
}

struct CoarseSecondsClock {
    typedef std::chrono::seconds duration;
    typedef duration::rep rep;
    typedef duration::period period;
    typedef std::chrono::time_point<CoarseSecondsClock, duration> time_point;
    static const bool is_steady = true;
    static std::atomic<std::int64_t> now_s;
    static time_point now() noexcept {
        return time_point(duration(now_s.load(std::memory_order_acquire)));
    }
};
std::atomic<std::int64_t> CoarseSecondsClock::now_s(100);

void test_coarse_clock_ttl_ceil() {
    CoarseSecondsClock::now_s.store(100, std::memory_order_release);
    ShardedTtlCache<std::string, int, std::hash<std::string>,
                    std::equal_to<std::string>, CoarseSecondsClock>
        cache(16, 2);
    // 500ms TTL on a 1-second clock rounds UP to 1 second (not 0 -> immediate expiry!)
    CHECK(cache.put("subtick", 42, 500));
    int v = 0;
    CHECK(cache.get("subtick", v) && v == 42);
    CoarseSecondsClock::now_s.store(101, std::memory_order_release);
    CHECK(!cache.get("subtick", v));
}

} // namespace

int main() {
    test_radix_and_suffix();
    test_aho_corasick();
    test_bloom_filter();
    test_inverted_index();
    test_top_k();
    test_sharded_ttl_cache();
    test_consistent_hash();
    test_token_bucket();
    test_double_buffer();
    test_radix_alloc_safety();
    test_aho_corasick_failure_states();
    test_sharded_ttl_cache_extremes();
    test_consistent_hash_hardening();
    test_review_growth_and_remove_rollback();
    test_claude_audit_p1_p2_regressions();
    test_coarse_clock_ttl_ceil();
    std::cout << "[PASS] test_search_and_ads_utils (" << g_checks.load()
              << " checks passed)" << std::endl;
    return 0;
}
