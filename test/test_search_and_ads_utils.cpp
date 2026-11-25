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
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
    std::cout << "[PASS] test_search_and_ads_utils (" << g_checks.load()
              << " checks passed)" << std::endl;
    return 0;
}
