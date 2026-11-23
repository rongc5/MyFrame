#include "outbound_guard.h"
#include <cstdlib>

namespace myframe {
namespace {

size_t env_default_hwm() {
    static const size_t v = [] {
        const char* e = std::getenv("MYFRAME_OUTBOUND_HWM_BYTES");
        if (e && *e) {
            char* end = nullptr;
            unsigned long long parsed = std::strtoull(e, &end, 10);
            if (end != e && parsed > 0) return static_cast<size_t>(parsed);
        }
        return kDefaultOutboundHighWaterMark;
    }();
    return v;
}

std::atomic<size_t> g_hwm_override{0};

struct Counters {
    std::atomic<uint64_t> slow_closed[2];
    std::atomic<uint64_t> dropped_msgs[2];
    std::atomic<uint64_t> dropped_bytes[2];
};

Counters& counters() {
    static Counters c{};
    return c;
}

} // namespace

size_t outbound_high_water_mark() {
    size_t v = g_hwm_override.load(std::memory_order_relaxed);
    return v ? v : env_default_hwm();
}

void set_outbound_high_water_mark(size_t bytes) {
    g_hwm_override.store(bytes, std::memory_order_relaxed);
}

OutboundStats outbound_stats() {
    Counters& c = counters();
    OutboundStats s;
    s.ws_slow_consumer_closed = c.slow_closed[0].load(std::memory_order_relaxed);
    s.ws_dropped_messages = c.dropped_msgs[0].load(std::memory_order_relaxed);
    s.ws_dropped_bytes = c.dropped_bytes[0].load(std::memory_order_relaxed);
    s.stream_slow_consumer_closed = c.slow_closed[1].load(std::memory_order_relaxed);
    s.stream_dropped_messages = c.dropped_msgs[1].load(std::memory_order_relaxed);
    s.stream_dropped_bytes = c.dropped_bytes[1].load(std::memory_order_relaxed);
    return s;
}

void reset_outbound_stats() {
    Counters& c = counters();
    for (int i = 0; i < 2; ++i) {
        c.slow_closed[i].store(0, std::memory_order_relaxed);
        c.dropped_msgs[i].store(0, std::memory_order_relaxed);
        c.dropped_bytes[i].store(0, std::memory_order_relaxed);
    }
}

namespace detail {

void note_dropped(OutboundKind kind, size_t bytes) {
    Counters& c = counters();
    int k = static_cast<int>(kind);
    c.dropped_msgs[k].fetch_add(1, std::memory_order_relaxed);
    c.dropped_bytes[k].fetch_add(bytes, std::memory_order_relaxed);
}

void note_slow_consumer_closed(OutboundKind kind) {
    counters().slow_closed[static_cast<int>(kind)].fetch_add(1, std::memory_order_relaxed);
}

} // namespace detail
} // namespace myframe
