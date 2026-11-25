#include "containers/containers.h"
#include "btree_map.h"
#include <stx/btree_multimap.h>
#include <stx/btree_set.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using myframe::containers::LruCache;
using myframe::containers::ShardedMap;

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

struct ThrowValue {
    static bool should_throw;
    int v;
    explicit ThrowValue(int x) : v(x) {
        if (should_throw) {
            throw std::runtime_error("value ctor failed");
        }
    }
};
bool ThrowValue::should_throw = false;

// ---------------------------------------------------------------------------
// 1. STX B+ Tree verification (both shim "btree_map.h" and <stx/...>)
// ---------------------------------------------------------------------------

void test_stx_btree() {
    stx::btree_map<int, std::string> bmap;
    for (int i = 0; i < 2000; ++i) {
        const int key = (i * 73) % 2000;
        bmap.insert(std::make_pair(key, "val_" + std::to_string(key)));
    }
    CHECK(bmap.size() == 2000);

    // Ordered range scan verification
    int expected = 0;
    for (stx::btree_map<int, std::string>::const_iterator it = bmap.begin();
         it != bmap.end();
         ++it) {
        CHECK(it->first == expected);
        CHECK(it->second == "val_" + std::to_string(expected));
        ++expected;
    }
    CHECK(expected == 2000);

    // Erase even keys
    for (int i = 0; i < 2000; i += 2) {
        CHECK(bmap.erase(i) == 1);
    }
    CHECK(bmap.size() == 1000);
    for (int i = 0; i < 2000; ++i) {
        stx::btree_map<int, std::string>::const_iterator it = bmap.find(i);
        if (i % 2 == 0) {
            CHECK(it == bmap.end());
        } else {
            CHECK(it != bmap.end() && it->second == "val_" + std::to_string(i));
        }
    }

    stx::btree_set<int> bset;
    for (int i = 0; i < 500; ++i) {
        bset.insert(i % 100);
    }
    CHECK(bset.size() == 100);

    stx::btree_multimap<int, int> bmmap;
    bmmap.insert(std::make_pair(7, 1));
    bmmap.insert(std::make_pair(7, 2));
    bmmap.insert(std::make_pair(7, 3));
    CHECK(bmmap.count(7) == 3);
}

// ---------------------------------------------------------------------------
// 2. LruCache unit & edge-case tests
// ---------------------------------------------------------------------------

void test_lru_cache() {
    bool threw = false;
    try {
        LruCache<int, int> bad(0);
        (void)bad;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    // Capacity 1 boundary
    {
        LruCache<std::string, int> c(1);
        CHECK(c.capacity() == 1 && c.empty() && c.size() == 0);
        CHECK(c.put("a", 1));
        CHECK(!c.put("a", 2));
        int v = 0;
        CHECK(c.get("a", v) && v == 2);
        CHECK(c.put("b", 3));
        CHECK(c.size() == 1);
        CHECK(!c.contains("a"));
        CHECK(c.get("b", v) && v == 3);
    }

    // Eviction order, MRU promotion, peek_ptr non-promotion, erase, clear
    {
        LruCache<int, std::string> c(3);
        CHECK(c.put(1, "one"));
        CHECK(c.put(2, "two"));
        CHECK(c.put(3, "three"));
        CHECK(c.size() == 3);

        // peek_ptr must NOT promote key 1
        const std::string* p1 = c.peek_ptr(1);
        CHECK(p1 != nullptr && *p1 == "one");

        // get promotes key 1 -> LRU order becomes [1, 3, 2]
        std::string out;
        CHECK(c.get(1, out) && out == "one");

        // Inserting 4 evicts 2
        CHECK(c.put(4, "four"));
        CHECK(!c.contains(2));
        CHECK(c.contains(1) && c.contains(3) && c.contains(4));

        // get_ptr promotes 3 and mutates in-place
        std::string* p3 = c.get_ptr(3);
        CHECK(p3 != nullptr && *p3 == "three");
        *p3 = "THREE";

        // Inserting 5 evicts 1 (order was [3, 4, 1])
        CHECK(c.put(5, "five"));
        CHECK(!c.contains(1));
        CHECK(c.get(3, out) && out == "THREE");

        CHECK(c.erase(4));
        CHECK(!c.erase(4));
        CHECK(c.size() == 2);

        c.clear();
        CHECK(c.empty() && c.size() == 0);
        CHECK(c.peek_ptr(3) == nullptr);
    }

    // Move-only values via emplace + get_ptr
    {
        LruCache<int, std::unique_ptr<int> > c(2);
        CHECK(c.emplace(1, new int(10)));
        CHECK(c.put(2, std::unique_ptr<int>(new int(20))));
        std::unique_ptr<int>* p = c.get_ptr(1);
        CHECK(p != nullptr && **p == 10);
        CHECK(c.emplace(3, new int(30)));
        CHECK(!c.contains(2));
        CHECK(c.contains(1) && c.contains(3));
    }

    // Exception rollback leaves cache consistent
    {
        LruCache<int, ThrowValue> c(2);
        ThrowValue::should_throw = false;
        CHECK(c.emplace(1, 100));
        ThrowValue::should_throw = true;
        threw = false;
        try {
            (void)c.emplace(2, 200);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(c.size() == 1 && c.contains(1) && !c.contains(2));
        ThrowValue::should_throw = false;
    }

    // Large logical capacity must not pre-allocate gigabytes up front
    {
        LruCache<int, int> big(10000000);
        CHECK(big.capacity() == 10000000);
        CHECK(big.put(1, 42));
        int out = 0;
        CHECK(big.get(1, out) && out == 42);
    }
}

// ---------------------------------------------------------------------------
// 3. ShardedMap unit & multi-threaded exact-accounting stress tests
// ---------------------------------------------------------------------------

void test_sharded_map() {
    bool threw = false;
    try {
        ShardedMap<int, int> bad(0);
        (void)bad;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    threw = false;
    try {
        ShardedMap<int, int> bad(100000);
        (void)bad;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    // Power-of-two shard rounding & basic operations
    {
        ShardedMap<std::string, int> m(13);
        CHECK(m.shard_count() == 16);
        CHECK(m.size() == 0);

        CHECK(m.put("k1", 10));
        CHECK(!m.put("k1", 20));
        CHECK(!m.insert_if_absent("k1", 30));
        CHECK(m.insert_if_absent("k2", 40));

        int out = 0;
        CHECK(m.get("k1", out) && out == 20);
        CHECK(m.get("k2", out) && out == 40);
        CHECK(!m.get("k3", out));
        CHECK(m.contains("k1") && !m.contains("missing"));

        CHECK(m.visit("k1", [](int& v) { v += 5; }));
        CHECK(!m.visit("missing", [](int& v) { v += 5; }));
        CHECK(m.get("k1", out) && out == 25);

        m.upsert("k3", [](int& v, bool inserted) {
            CHECK(inserted);
            v = 99;
        });
        m.upsert("k3", [](int& v, bool inserted) {
            CHECK(!inserted);
            v += 1;
        });
        CHECK(m.get("k3", out) && out == 100);

        int sum = 0;
        m.for_each([&sum](const std::string&, const int& v) {
            sum += v;
        });
        CHECK(sum == 25 + 40 + 100);

        CHECK(m.erase("k2"));
        CHECK(!m.erase("k2"));
        CHECK(m.size() == 2);
        m.clear();
        CHECK(m.size() == 0);
    }

    // Move-only values with put + visit
    {
        ShardedMap<int, std::unique_ptr<int> > m(8);
        CHECK(m.put(1, std::unique_ptr<int>(new int(77))));
        int seen = 0;
        CHECK(m.visit(1, [&seen](const std::unique_ptr<int>& p) {
            seen = *p;
        }));
        CHECK(seen == 77);
    }

    // Non-default-constructible Hash & KeyEqual + upsert throw rollback
    {
        struct NonDefaultHash {
            std::size_t seed;
            explicit NonDefaultHash(std::size_t s) : seed(s) {}
            std::size_t operator()(int k) const {
                return std::hash<int>()(k) ^ seed;
            }
        };
        struct NonDefaultEq {
            int tag;
            explicit NonDefaultEq(int t) : tag(t) {}
            bool operator()(int a, int b) const {
                (void)tag;
                return a == b;
            }
        };

        ShardedMap<int, int, NonDefaultHash, NonDefaultEq> m(
            8, NonDefaultHash(0x12345), NonDefaultEq(7));
        CHECK(m.put(10, 100));
        int v = 0;
        CHECK(m.get(10, v) && v == 100);

        bool upsert_threw = false;
        try {
            m.upsert(20, [](int&, bool inserted) {
                CHECK(inserted);
                throw std::runtime_error("upsert failed on fresh insert");
            });
        } catch (const std::runtime_error&) {
            upsert_threw = true;
        }
        CHECK(upsert_threw);
        CHECK(!m.contains(20)); // Rolled back!
        CHECK(m.size() == 1);
    }

    // Concurrent multi-threaded exact-accounting upsert stress
    {
        ShardedMap<int, std::uint64_t> m(64);
        const std::size_t kThreads = 8;
        const std::size_t kKeys = 256;
        const std::size_t kOpsPerThread = 20000;

        std::vector<std::thread> workers;
        for (std::size_t t = 0; t < kThreads; ++t) {
            workers.emplace_back([&, t]() {
                for (std::size_t i = 0; i < kOpsPerThread; ++i) {
                    const int key = static_cast<int>((t * 31 + i) % kKeys);
                    m.upsert(key, [](std::uint64_t& count, bool) {
                        ++count;
                    });
                }
            });
        }
        for (std::size_t i = 0; i < workers.size(); ++i) {
            workers[i].join();
        }

        CHECK(m.size() == kKeys);
        std::uint64_t total = 0;
        m.for_each([&total](const int&, const std::uint64_t& v) {
            total += v;
        });
        CHECK(total == static_cast<std::uint64_t>(kThreads * kOpsPerThread));
    }

    // Concurrent insert/get/erase/for_each churn (TSan & ASan verification)
    {
        ShardedMap<int, std::string> m(32);
        const std::size_t kThreads = 6;
        const std::size_t kIters = 10000;

        std::vector<std::thread> workers;
        for (std::size_t t = 0; t < kThreads; ++t) {
            workers.emplace_back([&, t]() {
                for (std::size_t i = 0; i < kIters; ++i) {
                    const int key = static_cast<int>(t * 100 + (i % 100));
                    if (i % 3 == 0) {
                        m.put(key, "v" + std::to_string(i));
                    } else if (i % 3 == 1) {
                        std::string out;
                        (void)m.get(key, out);
                    } else {
                        (void)m.erase(key);
                    }
                    if (i % 500 == 0) {
                        std::size_t cnt = 0;
                        m.for_each([&cnt](const int&, const std::string&) { ++cnt; });
                    }
                }
            });
        }
        for (std::size_t i = 0; i < workers.size(); ++i) {
            workers[i].join();
        }

        // Verify each thread's disjoint key range ends in exact deterministic state
        // Last iteration for each (t, r) where i = 0..kIters-1 and i % 100 == r:
        // kIters = 10000, so last i for residue r is 9900 + r.
        for (std::size_t t = 0; t < kThreads; ++t) {
            for (std::size_t r = 0; r < 100; ++r) {
                const std::size_t last_i = 9900 + r;
                const int key = static_cast<int>(t * 100 + r);
                std::string out;
                const bool present = m.get(key, out);
                if (last_i % 3 == 2) {
                    CHECK(!present);
                } else {
                    CHECK(present);
                }
            }
        }
    }
}

} // namespace

int main() {
    test_stx_btree();
    test_lru_cache();
    test_sharded_map();
    std::cout << "[PASS] test_utils_containers (" << g_checks.load()
              << " checks passed)" << std::endl;
    return 0;
}
