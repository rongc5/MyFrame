#include "lockfree/lockfree.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace myframe {
namespace lockfree {
namespace detail {

struct MpmcQueueTestAccess {
    template <typename T>
    static std::unique_ptr<MpmcQueue<T> > create_with_initial_pos(
        std::size_t requested_capacity,
        std::size_t initial_pos) {
        return std::unique_ptr<MpmcQueue<T> >(
            new MpmcQueue<T>(requested_capacity, initial_pos));
    }
};

} // namespace detail
} // namespace lockfree
} // namespace myframe

namespace {

using myframe::lockfree::MpmcQueue;
using myframe::lockfree::SpscQueue;
using myframe::lockfree::detail::MpmcQueueTestAccess;

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
static const bool kUnderSanitizer = true;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
static const bool kUnderSanitizer = true;
#else
static const bool kUnderSanitizer = false;
#endif
#else
static const bool kUnderSanitizer = false;
#endif

static std::size_t stress_scale(std::size_t base_release, std::size_t base_sanitizer) {
    const char* env = std::getenv("MYFRAME_LOCKFREE_STRESS_SCALE");
    std::size_t base = kUnderSanitizer ? base_sanitizer : base_release;
    if (env != nullptr && env[0] != '\0') {
        long mult = std::strtol(env, nullptr, 10);
        if (mult > 0 && mult <= 100) {
            return base * static_cast<std::size_t>(mult);
        }
    }
    return base;
}

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

// ---------------------------------------------------------------------------
// Helper types for lifetime, move-only, alignment, and exception injection
// ---------------------------------------------------------------------------

struct Tracked {
    static std::atomic<int> constructed;
    static std::atomic<int> destroyed;
    static std::atomic<int> live;

    int value;
    bool moved_from;

    static void reset_counters() {
        constructed.store(0, std::memory_order_relaxed);
        destroyed.store(0, std::memory_order_relaxed);
        live.store(0, std::memory_order_relaxed);
    }

    explicit Tracked(int v) noexcept : value(v), moved_from(false) {
        constructed.fetch_add(1, std::memory_order_relaxed);
        live.fetch_add(1, std::memory_order_relaxed);
    }

    Tracked(const Tracked& other) : value(other.value), moved_from(false) {
        constructed.fetch_add(1, std::memory_order_relaxed);
        live.fetch_add(1, std::memory_order_relaxed);
    }

    Tracked(Tracked&& other) noexcept : value(other.value), moved_from(false) {
        other.moved_from = true;
        other.value = -1;
        constructed.fetch_add(1, std::memory_order_relaxed);
        live.fetch_add(1, std::memory_order_relaxed);
    }

    Tracked& operator=(const Tracked& other) {
        value = other.value;
        moved_from = false;
        return *this;
    }

    Tracked& operator=(Tracked&& other) noexcept {
        value = other.value;
        moved_from = false;
        other.moved_from = true;
        other.value = -1;
        return *this;
    }

    ~Tracked() noexcept {
        destroyed.fetch_add(1, std::memory_order_relaxed);
        live.fetch_sub(1, std::memory_order_relaxed);
    }
};

std::atomic<int> Tracked::constructed(0);
std::atomic<int> Tracked::destroyed(0);
std::atomic<int> Tracked::live(0);

struct NonDefaultMoveOnly {
    int id;
    std::string tag;

    NonDefaultMoveOnly(int i, std::string t) noexcept : id(i), tag(std::move(t)) {}
    NonDefaultMoveOnly(const NonDefaultMoveOnly&) = delete;
    NonDefaultMoveOnly& operator=(const NonDefaultMoveOnly&) = delete;
    NonDefaultMoveOnly(NonDefaultMoveOnly&& other) noexcept
        : id(other.id), tag(std::move(other.tag)) {
        other.id = -1;
    }
    NonDefaultMoveOnly& operator=(NonDefaultMoveOnly&& other) noexcept {
        id = other.id;
        tag = std::move(other.tag);
        other.id = -1;
        return *this;
    }
    ~NonDefaultMoveOnly() noexcept {}
};

struct alignas(64) OverAlignedPayload {
    std::uint64_t words[8];
};

struct ThrowOnCopy {
    static int copies_until_throw;
    static int move_assigns_until_throw;
    static int live;

    int value;

    explicit ThrowOnCopy(int v) : value(v) {
        ++live;
    }

    ThrowOnCopy(const ThrowOnCopy& other) : value(other.value) {
        if (copies_until_throw == 0) {
            throw std::runtime_error("injected copy failure");
        }
        if (copies_until_throw > 0) {
            --copies_until_throw;
        }
        ++live;
    }

    ThrowOnCopy(ThrowOnCopy&& other) noexcept : value(other.value) {
        other.value = -999;
        ++live;
    }

    ThrowOnCopy& operator=(const ThrowOnCopy& other) {
        if (copies_until_throw == 0) {
            throw std::runtime_error("injected copy-assign failure");
        }
        if (copies_until_throw > 0) {
            --copies_until_throw;
        }
        value = other.value;
        return *this;
    }

    ThrowOnCopy& operator=(ThrowOnCopy&& other) {
        if (move_assigns_until_throw == 0) {
            throw std::runtime_error("injected move-assign failure");
        }
        if (move_assigns_until_throw > 0) {
            --move_assigns_until_throw;
        }
        value = other.value;
        other.value = -999;
        return *this;
    }

    ~ThrowOnCopy() noexcept {
        --live;
    }
};

int ThrowOnCopy::copies_until_throw = -1;
int ThrowOnCopy::move_assigns_until_throw = -1;
int ThrowOnCopy::live = 0;

struct ThrowingEmplaceCtor {
    static bool should_throw;
    static int live;
    int value;

    explicit ThrowingEmplaceCtor(int v) : value(v) {
        if (should_throw) {
            throw std::runtime_error("injected emplace failure");
        }
        ++live;
    }
    ThrowingEmplaceCtor(ThrowingEmplaceCtor&& other) noexcept : value(other.value) {
        other.value = -1;
        ++live;
    }
    ThrowingEmplaceCtor& operator=(ThrowingEmplaceCtor&& other) noexcept {
        value = other.value;
        other.value = -1;
        return *this;
    }
    ~ThrowingEmplaceCtor() noexcept {
        --live;
    }
};

bool ThrowingEmplaceCtor::should_throw = false;
int ThrowingEmplaceCtor::live = 0;

// ---------------------------------------------------------------------------
// 1. Unit & edge-case tests: SpscQueue
// ---------------------------------------------------------------------------

void test_spsc_unit() {
    // Invalid capacities
    bool threw = false;
    try {
        SpscQueue<int> q(0);
        (void)q;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    threw = false;
    try {
        SpscQueue<int> q(std::numeric_limits<std::size_t>::max());
        (void)q;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    threw = false;
    try {
        SpscQueue<int> q(std::numeric_limits<std::size_t>::max() / 2);
        (void)q;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    // Verify dynamic allocation triggers no -Waligned-new warnings in C++11
    std::unique_ptr<SpscQueue<int> > heap_q(new SpscQueue<int>(16));
    CHECK(heap_q->capacity() == 16);
    CHECK(heap_q->empty_approx());
    std::shared_ptr<SpscQueue<int> > shared_q = std::make_shared<SpscQueue<int> >(8);
    CHECK(shared_q->capacity() == 8);

    // Exact capacity checks across 1, 2, 3, 5, 16, 100
    const std::size_t caps[] = {1, 2, 3, 5, 16, 100};
    for (std::size_t ci = 0; ci < sizeof(caps) / sizeof(caps[0]); ++ci) {
        const std::size_t cap = caps[ci];
        SpscQueue<int> q(cap);
        CHECK(q.capacity() == cap);
        CHECK(q.empty_approx());
        CHECK(q.size_approx() == 0);
        int out = -1;
        CHECK(!q.try_pop(out));
        CHECK(q.front() == nullptr);

        // Multiple full fill/drain laps to exercise wrap-around
        for (int lap = 0; lap < 5; ++lap) {
            for (std::size_t i = 0; i < cap; ++i) {
                CHECK(q.try_push(static_cast<int>(lap * 1000 + i)));
            }
            CHECK(q.size_approx() == cap);
            CHECK(!q.empty_approx());
            CHECK(!q.try_push(999999));

            for (std::size_t i = 0; i < cap; ++i) {
                CHECK(q.try_pop(out));
                CHECK(out == static_cast<int>(lap * 1000 + i));
            }
            CHECK(q.empty_approx());
            CHECK(q.size_approx() == 0);
            CHECK(!q.try_pop(out));
        }
    }

    // Move-only unique_ptr + rvalue intact on failure
    {
        SpscQueue<std::unique_ptr<int> > q(2);
        std::unique_ptr<int> p1(new int(11));
        std::unique_ptr<int> p2(new int(22));
        std::unique_ptr<int> p3(new int(33));
        CHECK(q.try_push(std::move(p1)));
        CHECK(p1 == nullptr);
        CHECK(q.try_push(std::move(p2)));
        CHECK(p2 == nullptr);
        // Queue is full: p3 MUST remain untouched!
        CHECK(!q.try_push(std::move(p3)));
        CHECK(p3 != nullptr && *p3 == 33);

        // Zero-copy front() + pop()
        std::unique_ptr<int>* front_ptr = q.front();
        CHECK(front_ptr != nullptr && **front_ptr == 11);
        q.pop();

        // try_consume
        int seen = 0;
        CHECK(q.try_consume([&seen](std::unique_ptr<int>&& v) {
            seen = *v;
        }));
        CHECK(seen == 22);
        CHECK(q.front() == nullptr);
    }

    // Non-default-constructible move-only type
    {
        SpscQueue<NonDefaultMoveOnly> q(3);
        CHECK(q.try_emplace(1, "alpha"));
        CHECK(q.try_emplace(2, "beta"));
        int got_id = 0;
        std::string got_tag;
        CHECK(q.try_consume([&](NonDefaultMoveOnly&& item) {
            got_id = item.id;
            got_tag = std::move(item.tag);
        }));
        CHECK(got_id == 1 && got_tag == "alpha");
    }

    // Over-aligned payload alignment guarantee
    {
        SpscQueue<OverAlignedPayload> q(7);
        OverAlignedPayload payload = {{1, 2, 3, 4, 5, 6, 7, 8}};
        for (int i = 0; i < 15; ++i) {
            CHECK(q.try_push(payload));
            OverAlignedPayload* fp = q.front();
            CHECK(fp != nullptr);
            CHECK((reinterpret_cast<std::uintptr_t>(fp) % 64) == 0);
            CHECK(fp->words[7] == 8);
            q.pop();
        }
    }

    // Bulk try_push_n / try_pop_n (copies + wrap-around + move_iterator unique_ptr)
    {
        SpscQueue<int> q(5);
        int in1[7] = {10, 20, 30, 40, 50, 60, 70};
        CHECK(q.try_push_n(in1, 7) == 5);
        CHECK(q.size_approx() == 5);
        CHECK(q.try_push_n(in1, 2) == 0);

        int out1[3] = {0, 0, 0};
        CHECK(q.try_pop_n(out1, 3) == 3);
        CHECK(out1[0] == 10 && out1[1] == 20 && out1[2] == 30);

        // Push across wrap boundary
        int in2[4] = {60, 70, 80, 90};
        CHECK(q.try_push_n(in2, 4) == 3);
        CHECK(q.size_approx() == 5);

        std::vector<int> drained;
        CHECK(q.try_pop_n(std::back_inserter(drained), 10) == 5);
        CHECK(drained.size() == 5);
        CHECK(drained[0] == 40 && drained[1] == 50 && drained[2] == 60 &&
              drained[3] == 70 && drained[4] == 80);
        CHECK(q.empty_approx());
    }

    {
        SpscQueue<std::unique_ptr<int> > q(4);
        std::vector<std::unique_ptr<int> > src;
        for (int i = 0; i < 4; ++i) {
            src.push_back(std::unique_ptr<int>(new int(100 + i)));
        }
        CHECK(q.try_push_n(std::make_move_iterator(src.begin()), src.size()) == 4);
        std::vector<std::unique_ptr<int> > dst(4);
        CHECK(q.try_pop_n(dst.begin(), 4) == 4);
        for (int i = 0; i < 4; ++i) {
            CHECK(dst[static_cast<std::size_t>(i)] != nullptr);
            CHECK(*dst[static_cast<std::size_t>(i)] == 100 + i);
        }
    }
}

// ---------------------------------------------------------------------------
// 2. Unit & edge-case tests: MpmcQueue
// ---------------------------------------------------------------------------

void test_mpmc_unit() {
    // Invalid capacities
    bool threw = false;
    try {
        MpmcQueue<int> q(0);
        (void)q;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    threw = false;
    try {
        MpmcQueue<int> q(std::numeric_limits<std::size_t>::max());
        (void)q;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    threw = false;
    try {
        MpmcQueue<int> q(std::numeric_limits<std::size_t>::max() / 2);
        (void)q;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    // Heap allocations prove zero -Waligned-new regression
    std::unique_ptr<MpmcQueue<int> > heap_q(new MpmcQueue<int>(16));
    CHECK(heap_q->capacity() == 16);
    std::shared_ptr<MpmcQueue<int> > shared_q = std::make_shared<MpmcQueue<int> >(3);
    CHECK(shared_q->capacity() == 4);

    // Capacity rounding verification
    const std::pair<std::size_t, std::size_t> cases[] = {
        {1, 2}, {2, 2}, {3, 4}, {4, 4}, {5, 8}, {9, 16}, {1000, 1024}
    };
    for (std::size_t ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ++ci) {
        MpmcQueue<int> q(cases[ci].first);
        CHECK(q.capacity() == cases[ci].second);
        CHECK(q.empty_approx());
        CHECK(q.size_approx() == 0);
        int out = -1;
        CHECK(!q.try_pop(out));

        for (int lap = 0; lap < 4; ++lap) {
            for (std::size_t i = 0; i < q.capacity(); ++i) {
                CHECK(q.try_push(static_cast<int>(lap * 10000 + i)));
            }
            CHECK(q.size_approx() == q.capacity());
            CHECK(!q.try_push(-1));

            for (std::size_t i = 0; i < q.capacity(); ++i) {
                CHECK(q.try_pop(out));
                CHECK(out == static_cast<int>(lap * 10000 + i));
            }
            CHECK(q.empty_approx());
            CHECK(!q.try_pop(out));
        }
    }

    // Move-only unique_ptr + rvalue intact when full
    {
        MpmcQueue<std::unique_ptr<int> > q(2);
        std::unique_ptr<int> p1(new int(7));
        std::unique_ptr<int> p2(new int(8));
        std::unique_ptr<int> p3(new int(9));
        CHECK(q.try_push(std::move(p1)));
        CHECK(p1 == nullptr);
        CHECK(q.try_push(std::move(p2)));
        CHECK(p2 == nullptr);
        CHECK(!q.try_push(std::move(p3)));
        CHECK(p3 != nullptr && *p3 == 9);

        std::unique_ptr<int> out;
        CHECK(q.try_pop(out) && *out == 7);
        CHECK(q.try_pop(out) && *out == 8);
        CHECK(!q.try_pop(out));
    }

    // Non-default-constructible move-only + over-aligned payload
    {
        MpmcQueue<NonDefaultMoveOnly> q(4);
        CHECK(q.try_emplace(42, "hello"));
        int got_id = 0;
        std::string got_tag;
        CHECK(q.try_consume([&](NonDefaultMoveOnly&& item) {
            got_id = item.id;
            got_tag = std::move(item.tag);
        }));
        CHECK(got_id == 42 && got_tag == "hello");
    }

    {
        MpmcQueue<OverAlignedPayload> q(4);
        OverAlignedPayload payload = {{10, 20, 30, 40, 50, 60, 70, 80}};
        for (int i = 0; i < 12; ++i) {
            CHECK(q.try_push(payload));
            CHECK(q.try_consume([](OverAlignedPayload&& item) {
                CHECK((reinterpret_cast<std::uintptr_t>(&item) % 64) == 0);
                CHECK(item.words[0] == 10 && item.words[7] == 80);
            }));
        }
    }
}

// ---------------------------------------------------------------------------
// 3. Lifetime & destructor accounting (partial drain + wrap-around)
// ---------------------------------------------------------------------------

void test_lifetime_accounting() {
    // SpscQueue: leave live items across wrap-around boundary on destruction
    Tracked::reset_counters();
    {
        SpscQueue<Tracked> q(5);
        for (int i = 0; i < 4; ++i) {
            CHECK(q.try_emplace(i));
        }
        Tracked out(-1);
        CHECK(q.try_pop(out) && out.value == 0);
        CHECK(q.try_pop(out) && out.value == 1);
        // Push 3 more so tail wraps past 0 while head is at 2
        CHECK(q.try_emplace(10));
        CHECK(q.try_emplace(11));
        CHECK(q.try_emplace(12));
        CHECK(q.size_approx() == 5);
        // Queue destroyed here with 5 live Tracked items inside
    }
    CHECK(Tracked::live.load() == 0);
    CHECK(Tracked::constructed.load() == Tracked::destroyed.load());

    // MpmcQueue: leave live items across wrap-around on destruction
    Tracked::reset_counters();
    {
        MpmcQueue<Tracked> q(4);
        for (int i = 0; i < 4; ++i) {
            CHECK(q.try_emplace(i));
        }
        Tracked out(-1);
        CHECK(q.try_pop(out) && out.value == 0);
        CHECK(q.try_pop(out) && out.value == 1);
        CHECK(q.try_emplace(20));
        CHECK(q.try_emplace(21));
        CHECK(q.size_approx() == 4);
    }
    CHECK(Tracked::live.load() == 0);
    CHECK(Tracked::constructed.load() == Tracked::destroyed.load());
}

// ---------------------------------------------------------------------------
// 4. Exception injection (strong/basic guarantees, zero wedge, zero leak)
// ---------------------------------------------------------------------------

void test_exception_safety() {
    // SpscQueue: throwing copy in try_push, mid-batch try_push_n, try_pop, try_consume
    ThrowOnCopy::copies_until_throw = -1;
    ThrowOnCopy::move_assigns_until_throw = -1;
    ThrowOnCopy::live = 0;
    {
        SpscQueue<ThrowOnCopy> q(4);
        ThrowOnCopy a(1), b(2), c(3);
        CHECK(q.try_push(a));

        ThrowOnCopy::copies_until_throw = 0;
        bool threw = false;
        try {
            (void)q.try_push(b);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(q.size_approx() == 1); // Strong guarantee

        // Mid-batch rollback in try_push_n: 3 items requested, throw on 3rd copy
        ThrowOnCopy::copies_until_throw = 2;
        ThrowOnCopy batch[3] = {ThrowOnCopy(10), ThrowOnCopy(20), ThrowOnCopy(30)};
        threw = false;
        try {
            (void)q.try_push_n(batch, 3);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(q.size_approx() == 1); // 2 constructed slots rolled back!

        ThrowOnCopy::copies_until_throw = -1;
        CHECK(q.try_push(b));
        CHECK(q.try_push(c));
        CHECK(q.size_approx() == 3);

        // Throwing move-assignment in try_pop keeps element queued
        ThrowOnCopy out(-1);
        ThrowOnCopy::move_assigns_until_throw = 0;
        threw = false;
        try {
            (void)q.try_pop(out);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(q.size_approx() == 3);

        ThrowOnCopy::move_assigns_until_throw = -1;
        CHECK(q.try_pop(out) && out.value == 1);

        // Throwing callback in try_consume still destroys & advances
        threw = false;
        try {
            (void)q.try_consume([](ThrowOnCopy&& item) {
                CHECK(item.value == 2);
                throw std::runtime_error("consumer threw");
            });
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(q.size_approx() == 1);
        CHECK(q.try_pop(out) && out.value == 3);
        CHECK(q.empty_approx());
    }
    CHECK(ThrowOnCopy::live == 0);

    // Regression: throwing ++out on the FIRST element of SpscQueue::try_pop_n
    // must still advance head_ so the already-destroyed element is never double-destroyed.
    Tracked::reset_counters();
    {
        struct ThrowingIncOutputIt {
            Tracked* dst;
            int count;
            Tracked& operator*() { return *dst; }
            ThrowingIncOutputIt& operator++() {
                ++count;
                throw std::runtime_error("iterator increment threw on first item");
            }
        };

        SpscQueue<Tracked> q(4);
        CHECK(q.try_emplace(101));
        CHECK(q.try_emplace(102));
        Tracked sink(-1);
        ThrowingIncOutputIt bad_it = {&sink, 0};
        bool threw = false;
        try {
            (void)q.try_pop_n(bad_it, 2);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(sink.value == 101);
        CHECK(q.size_approx() == 1);
        Tracked next(-1);
        CHECK(q.try_pop(next) && next.value == 102);
        CHECK(q.empty_approx());
    }
    CHECK(Tracked::live.load() == 0);
    CHECK(Tracked::constructed.load() == Tracked::destroyed.load());

    // MpmcQueue: throwing copy in try_push + throwing emplace + throwing consume/pop
    ThrowOnCopy::copies_until_throw = -1;
    ThrowOnCopy::move_assigns_until_throw = -1;
    ThrowOnCopy::live = 0;
    {
        MpmcQueue<ThrowOnCopy> q(4);
        ThrowOnCopy a(10), b(20), c(30);
        CHECK(q.try_push(a));

        // Copy throws BEFORE slot claim -> queue must NOT wedge!
        ThrowOnCopy::copies_until_throw = 0;
        bool threw = false;
        try {
            (void)q.try_push(b);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(q.size_approx() == 1);

        ThrowOnCopy::copies_until_throw = -1;
        CHECK(q.try_push(b));
        CHECK(q.try_push(c));
        CHECK(q.size_approx() == 3);

        // Throwing try_consume must release slot via ReleaseGuard (no wedge)
        threw = false;
        try {
            (void)q.try_consume([](ThrowOnCopy&& item) {
                CHECK(item.value == 10);
                throw std::runtime_error("mpmc consume boom");
            });
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);

        // Throwing move-assignment in try_pop must release slot (basic guarantee)
        ThrowOnCopy out(-1);
        ThrowOnCopy::move_assigns_until_throw = 0;
        threw = false;
        try {
            (void)q.try_pop(out);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);

        ThrowOnCopy::move_assigns_until_throw = -1;
        CHECK(q.try_pop(out) && out.value == 30);
        CHECK(q.empty_approx());

        // Verify queue wraps cleanly for multiple full laps after all exceptions
        for (int i = 0; i < 20; ++i) {
            CHECK(q.try_push(ThrowOnCopy(i)));
            CHECK(q.try_pop(out) && out.value == i);
        }
    }
    CHECK(ThrowOnCopy::live == 0);

    // MpmcQueue: throwing try_emplace (non-copyable, throwing ctor)
    ThrowingEmplaceCtor::should_throw = false;
    ThrowingEmplaceCtor::live = 0;
    {
        MpmcQueue<ThrowingEmplaceCtor> q(2);
        CHECK(q.try_emplace(1));
        ThrowingEmplaceCtor::should_throw = true;
        bool threw = false;
        try {
            (void)q.try_emplace(2);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
        ThrowingEmplaceCtor::should_throw = false;
        CHECK(q.try_emplace(2));
        CHECK(!q.try_emplace(3)); // Full!

        ThrowingEmplaceCtor out(0);
        CHECK(q.try_pop(out) && out.value == 1);
        CHECK(q.try_pop(out) && out.value == 2);
        CHECK(q.empty_approx());
    }
    CHECK(ThrowingEmplaceCtor::live == 0);
}

// ---------------------------------------------------------------------------
// 5. Deterministic SIZE_MAX modular index wrap-around test for MpmcQueue
// ---------------------------------------------------------------------------

void test_mpmc_index_wraparound() {
    const std::size_t kMax = std::numeric_limits<std::size_t>::max();
    const std::size_t caps[] = {2, 4, 8};

    for (std::size_t ci = 0; ci < sizeof(caps) / sizeof(caps[0]); ++ci) {
        const std::size_t cap = caps[ci];
        const std::size_t start_pos = kMax - 3;

        // Single-threaded full wrap across SIZE_MAX -> 0
        {
            std::unique_ptr<MpmcQueue<std::uint64_t> > q =
                MpmcQueueTestAccess::create_with_initial_pos<std::uint64_t>(cap, start_pos);
            CHECK(q->empty_approx());
            CHECK(q->size_approx() == 0);

            for (std::uint64_t round = 0; round < 32; ++round) {
                for (std::size_t i = 0; i < cap; ++i) {
                    CHECK(q->try_push(round * 100 + i));
                }
                CHECK(q->size_approx() == cap);
                CHECK(!q->try_push(999999));

                for (std::size_t i = 0; i < cap; ++i) {
                    std::uint64_t v = 0;
                    CHECK(q->try_pop(v));
                    CHECK(v == round * 100 + i);
                }
                CHECK(q->empty_approx());
            }
        }

        // Destruction with live Tracked objects straddling SIZE_MAX -> 0
        Tracked::reset_counters();
        {
            std::unique_ptr<MpmcQueue<Tracked> > q =
                MpmcQueueTestAccess::create_with_initial_pos<Tracked>(cap, kMax - 1);
            for (std::size_t i = 0; i < cap; ++i) {
                CHECK(q->try_emplace(static_cast<int>(i)));
            }
            CHECK(q->size_approx() == cap);
            // Straddles SIZE_MAX - 1, SIZE_MAX, 0, 1...
        }
        CHECK(Tracked::live.load() == 0);
        CHECK(Tracked::constructed.load() == Tracked::destroyed.load());
    }

    // Concurrent 4P x 4C stress starting right before SIZE_MAX
    {
        const std::size_t kProducers = 4;
        const std::size_t kConsumers = 4;
        const std::size_t kPerProd = stress_scale(50000, 10000);
        const std::size_t kTotal = kProducers * kPerProd;

        std::unique_ptr<MpmcQueue<std::uint64_t> > q =
            MpmcQueueTestAccess::create_with_initial_pos<std::uint64_t>(4, kMax - 17);

        std::vector<std::vector<std::uint8_t> > seen(
            kProducers, std::vector<std::uint8_t>(kPerProd, 0));
        std::atomic<std::size_t> consumed(0);
        std::vector<std::vector<std::uint64_t> > per_consumer(kConsumers);

        std::vector<std::thread> producers;
        std::vector<std::thread> consumers;

        for (std::size_t p = 0; p < kProducers; ++p) {
            producers.emplace_back([&, p]() {
                for (std::size_t seq = 0; seq < kPerProd; ++seq) {
                    const std::uint64_t val =
                        (static_cast<std::uint64_t>(p) << 32) |
                        static_cast<std::uint64_t>(seq);
                    while (!q->try_push(val)) {
                        std::this_thread::yield();
                    }
                }
            });
        }

        for (std::size_t c = 0; c < kConsumers; ++c) {
            consumers.emplace_back([&, c]() {
                std::vector<std::uint64_t>& local = per_consumer[c];
                local.reserve(kTotal / kConsumers + 1024);
                std::uint64_t val = 0;
                while (consumed.load(std::memory_order_acquire) < kTotal) {
                    if (q->try_pop(val)) {
                        local.push_back(val);
                        consumed.fetch_add(1, std::memory_order_release);
                    } else {
                        std::this_thread::yield();
                    }
                }
            });
        }

        for (std::size_t i = 0; i < producers.size(); ++i) producers[i].join();
        for (std::size_t i = 0; i < consumers.size(); ++i) consumers[i].join();

        CHECK(consumed.load() == kTotal);
        CHECK(q->empty_approx());

        std::size_t total_verified = 0;
        for (std::size_t c = 0; c < kConsumers; ++c) {
            std::vector<std::int64_t> last_seq(kProducers, -1);
            for (std::size_t i = 0; i < per_consumer[c].size(); ++i) {
                const std::uint64_t v = per_consumer[c][i];
                const std::size_t p = static_cast<std::size_t>(v >> 32);
                const std::size_t s = static_cast<std::size_t>(v & 0xffffffffULL);
                CHECK(p < kProducers);
                CHECK(s < kPerProd);
                CHECK(static_cast<std::int64_t>(s) > last_seq[p]);
                last_seq[p] = static_cast<std::int64_t>(s);
                CHECK(seen[p][s] == 0);
                seen[p][s] = 1;
                ++total_verified;
            }
        }
        CHECK(total_verified == kTotal);
        for (std::size_t p = 0; p < kProducers; ++p) {
            for (std::size_t s = 0; s < kPerProd; ++s) {
                CHECK(seen[p][s] == 1);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 6. Concurrent SPSC stress tests (strict total FIFO + bulk + unique_ptr)
// ---------------------------------------------------------------------------

void run_spsc_stress_case(std::size_t capacity, std::size_t count) {
    SpscQueue<std::uint64_t> q(capacity);
    std::thread prod([&]() {
        for (std::uint64_t i = 0; i < count; ++i) {
            while (!q.try_push(i)) {
                std::this_thread::yield();
            }
        }
    });
    std::thread cons([&]() {
        std::uint64_t expected = 0;
        std::uint64_t val = 0;
        while (expected < count) {
            if (q.try_pop(val)) {
                CHECK(val == expected);
                ++expected;
            } else {
                std::this_thread::yield();
            }
        }
    });
    prod.join();
    cons.join();
    CHECK(q.empty_approx());
    CHECK(q.size_approx() == 0);
}

void test_spsc_concurrency() {
    run_spsc_stress_case(1, stress_scale(100000, 20000));
    run_spsc_stress_case(2, stress_scale(300000, 40000));
    run_spsc_stress_case(64, stress_scale(500000, 60000));

    // Bulk SPSC stress with varying batch sizes across wrap boundaries
    {
        const std::size_t kCount = stress_scale(400000, 50000);
        SpscQueue<std::uint64_t> q(127); // prime non-power-of-two capacity
        std::thread prod([&]() {
            std::uint64_t next = 0;
            std::uint64_t buf[37];
            while (next < kCount) {
                std::size_t batch = static_cast<std::size_t>((next % 37) + 1);
                if (next + batch > kCount) {
                    batch = static_cast<std::size_t>(kCount - next);
                }
                for (std::size_t i = 0; i < batch; ++i) {
                    buf[i] = next + i;
                }
                std::size_t pushed = 0;
                while (pushed < batch) {
                    std::size_t n = q.try_push_n(buf + pushed, batch - pushed);
                    if (n == 0) {
                        std::this_thread::yield();
                    } else {
                        pushed += n;
                    }
                }
                next += batch;
            }
        });
        std::thread cons([&]() {
            std::uint64_t expected = 0;
            std::uint64_t buf[29];
            while (expected < kCount) {
                std::size_t want = static_cast<std::size_t>((expected % 29) + 1);
                std::size_t n = q.try_pop_n(buf, want);
                if (n == 0) {
                    std::this_thread::yield();
                } else {
                    for (std::size_t i = 0; i < n; ++i) {
                        CHECK(buf[i] == expected);
                        ++expected;
                    }
                }
            }
        });
        prod.join();
        cons.join();
        CHECK(q.empty_approx());
    }

    // Move-only unique_ptr SPSC with exact lifetime accounting
    Tracked::reset_counters();
    {
        const std::size_t kCount = stress_scale(100000, 20000);
        SpscQueue<std::unique_ptr<Tracked> > q(16);
        std::thread prod([&]() {
            for (std::size_t i = 0; i < kCount; ++i) {
                std::unique_ptr<Tracked> item(new Tracked(static_cast<int>(i)));
                while (!q.try_push(std::move(item))) {
                    CHECK(item != nullptr); // Not moved from when full
                    std::this_thread::yield();
                }
                CHECK(item == nullptr);
            }
        });
        std::thread cons([&]() {
            std::size_t expected = 0;
            while (expected < kCount) {
                std::unique_ptr<Tracked>* fp = q.front();
                if (fp != nullptr) {
                    CHECK(*fp != nullptr);
                    CHECK((*fp)->value == static_cast<int>(expected));
                    q.pop();
                    ++expected;
                } else {
                    std::this_thread::yield();
                }
            }
        });
        prod.join();
        cons.join();
        CHECK(q.empty_approx());
    }
    CHECK(Tracked::live.load() == 0);
    CHECK(Tracked::constructed.load() == Tracked::destroyed.load());
}

// ---------------------------------------------------------------------------
// 7. Concurrent MPMC exact-accounting & per-producer monotonicity matrix
// ---------------------------------------------------------------------------

void run_mpmc_stress_case(std::size_t capacity,
                          std::size_t num_producers,
                          std::size_t num_consumers,
                          std::size_t items_per_producer) {
    const std::size_t total_items = num_producers * items_per_producer;
    MpmcQueue<std::uint64_t> q(capacity);

    std::atomic<std::size_t> consumed_total(0);
    std::vector<std::vector<std::uint64_t> > per_consumer(num_consumers);

    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;

    for (std::size_t p = 0; p < num_producers; ++p) {
        producers.emplace_back([&, p]() {
            for (std::size_t seq = 0; seq < items_per_producer; ++seq) {
                const std::uint64_t val =
                    (static_cast<std::uint64_t>(p) << 32) |
                    static_cast<std::uint64_t>(seq);
                while (!q.try_push(val)) {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (std::size_t c = 0; c < num_consumers; ++c) {
        consumers.emplace_back([&, c]() {
            std::vector<std::uint64_t>& local = per_consumer[c];
            local.reserve(total_items / num_consumers + 1024);
            std::uint64_t val = 0;
            while (consumed_total.load(std::memory_order_acquire) < total_items) {
                if (q.try_pop(val)) {
                    local.push_back(val);
                    consumed_total.fetch_add(1, std::memory_order_release);
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (std::size_t i = 0; i < producers.size(); ++i) producers[i].join();
    for (std::size_t i = 0; i < consumers.size(); ++i) consumers[i].join();

    CHECK(consumed_total.load() == total_items);
    CHECK(q.empty_approx());
    CHECK(q.size_approx() == 0);

    // Exact per-item accounting (every (p, seq) delivered exactly once) +
    // per-consumer per-producer strict monotonicity check.
    std::vector<std::vector<std::uint8_t> > seen(
        num_producers, std::vector<std::uint8_t>(items_per_producer, 0));
    std::size_t total_verified = 0;

    for (std::size_t c = 0; c < num_consumers; ++c) {
        std::vector<std::int64_t> last_seq(num_producers, -1);
        const std::vector<std::uint64_t>& local = per_consumer[c];
        for (std::size_t i = 0; i < local.size(); ++i) {
            const std::uint64_t v = local[i];
            const std::size_t p = static_cast<std::size_t>(v >> 32);
            const std::size_t s = static_cast<std::size_t>(v & 0xffffffffULL);
            CHECK(p < num_producers);
            CHECK(s < items_per_producer);
            if (num_consumers == 1) {
                CHECK(static_cast<std::int64_t>(s) == last_seq[p] + 1);
            } else {
                CHECK(static_cast<std::int64_t>(s) > last_seq[p]);
            }
            last_seq[p] = static_cast<std::int64_t>(s);
            CHECK(seen[p][s] == 0);
            seen[p][s] = 1;
            ++total_verified;
        }
    }

    CHECK(total_verified == total_items);
    for (std::size_t p = 0; p < num_producers; ++p) {
        for (std::size_t s = 0; s < items_per_producer; ++s) {
            CHECK(seen[p][s] == 1);
        }
    }
}

void test_mpmc_concurrency() {
    const std::size_t per_p_small = stress_scale(80000, 12000);
    const std::size_t per_p_med = stress_scale(150000, 20000);

    // Capacity 2 forces constant wrap-around and maximum slot contention
    run_mpmc_stress_case(2, 1, 1, per_p_small);
    run_mpmc_stress_case(2, 4, 1, per_p_small); // MPSC
    run_mpmc_stress_case(2, 1, 4, per_p_small); // SPMC
    run_mpmc_stress_case(2, 4, 4, per_p_small); // MPMC high contention

    // Realistic capacities across MPSC / MPMC topologies
    run_mpmc_stress_case(64, 4, 1, per_p_med);
    run_mpmc_stress_case(64, 4, 4, per_p_med);
    run_mpmc_stress_case(128, 6, 6, stress_scale(100000, 15000));

    // Concurrent unique_ptr<Tracked> 4P x 4C stress verifying zero leaks
    Tracked::reset_counters();
    {
        const std::size_t kProducers = 4;
        const std::size_t kConsumers = 4;
        const std::size_t kPerProd = stress_scale(30000, 8000);
        const std::size_t kTotal = kProducers * kPerProd;

        MpmcQueue<std::unique_ptr<Tracked> > q(16);
        std::atomic<std::size_t> consumed(0);
        std::atomic<std::uint64_t> sum(0);

        std::vector<std::thread> producers;
        std::vector<std::thread> consumers;
        for (std::size_t p = 0; p < kProducers; ++p) {
            producers.emplace_back([&, p]() {
                for (std::size_t s = 0; s < kPerProd; ++s) {
                    const int v = static_cast<int>(p * kPerProd + s + 1);
                    std::unique_ptr<Tracked> ptr(new Tracked(v));
                    while (!q.try_push(std::move(ptr))) {
                        CHECK(ptr != nullptr); // Not moved from on failure
                        std::this_thread::yield();
                    }
                    CHECK(ptr == nullptr);
                }
            });
        }
        for (std::size_t c = 0; c < kConsumers; ++c) {
            consumers.emplace_back([&]() {
                while (consumed.load(std::memory_order_acquire) < kTotal) {
                    bool ok = q.try_consume([&](std::unique_ptr<Tracked>&& ptr) {
                        CHECK(ptr != nullptr);
                        sum.fetch_add(static_cast<std::uint64_t>(ptr->value),
                                      std::memory_order_relaxed);
                    });
                    if (ok) {
                        consumed.fetch_add(1, std::memory_order_release);
                    } else {
                        std::this_thread::yield();
                    }
                }
            });
        }
        for (std::size_t i = 0; i < producers.size(); ++i) producers[i].join();
        for (std::size_t i = 0; i < consumers.size(); ++i) consumers[i].join();

        CHECK(consumed.load() == kTotal);
        const std::uint64_t expected_sum =
            static_cast<std::uint64_t>(kTotal) * (kTotal + 1) / 2;
        CHECK(sum.load() == expected_sum);
        CHECK(q.empty_approx());
    }
    CHECK(Tracked::live.load() == 0);
    CHECK(Tracked::constructed.load() == Tracked::destroyed.load());
}

} // namespace

int main() {
    test_spsc_unit();
    test_mpmc_unit();
    test_lifetime_accounting();
    test_exception_safety();
    test_mpmc_index_wraparound();
    test_spsc_concurrency();
    test_mpmc_concurrency();
    std::cout << "[PASS] test_lockfree_queue (" << g_checks.load()
              << " checks passed)" << std::endl;
    return 0;
}
