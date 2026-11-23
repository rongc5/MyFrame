// Outbound backpressure: per-connection queued-byte accounting with a
// configurable high-water mark (HWM) plus process-wide slow-consumer counters.
//
// Every outbound byte accepted into a connection's application-level send
// queue (web_socket_data_process::_send_list, base_data_process::_send_list,
// BinaryContextImpl pending queue, or a cross-thread send message that is
// still in flight on the owner thread's channel) is reserved against the
// connection's OutboundState. Bytes are released when the socket layer pulls
// the buffer (get_send_buf) or when the buffer is discarded.
//
// When a reservation would push the queued bytes above the HWM the connection
// is treated as a slow consumer: the payload is dropped (counted) and the
// connection is closed cleanly on its owner thread, so RSS stays bounded even
// when a client stops reading.
#ifndef __MYFRAME_OUTBOUND_GUARD_H__
#define __MYFRAME_OUTBOUND_GUARD_H__

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace myframe {

// Default per-connection outbound high-water mark (4 MiB). Can be overridden
// with the MYFRAME_OUTBOUND_HWM_BYTES environment variable (read once) or at
// runtime with set_outbound_high_water_mark().
constexpr size_t kDefaultOutboundHighWaterMark = 4u * 1024u * 1024u;

size_t outbound_high_water_mark();
// 0 restores the default (environment variable or kDefaultOutboundHighWaterMark).
void set_outbound_high_water_mark(size_t bytes);

enum class OutboundKind {
    WebSocket = 0,   // WebSocket data frames (including WsPushHub pushes)
    Stream = 1,      // base_data_process queue: HTTP/2, raw/custom streams, binary
};

struct OutboundStats {
    uint64_t ws_slow_consumer_closed{0};
    uint64_t ws_dropped_messages{0};
    uint64_t ws_dropped_bytes{0};
    uint64_t stream_slow_consumer_closed{0};
    uint64_t stream_dropped_messages{0};
    uint64_t stream_dropped_bytes{0};

    uint64_t slow_consumer_closed() const { return ws_slow_consumer_closed + stream_slow_consumer_closed; }
    uint64_t dropped_messages() const { return ws_dropped_messages + stream_dropped_messages; }
    uint64_t dropped_bytes() const { return ws_dropped_bytes + stream_dropped_bytes; }
};

OutboundStats outbound_stats();
void reset_outbound_stats();

namespace detail {

// Shared, heap-allocated per-connection outbound state. It is owned by the
// process object (shared_ptr) and handed out to foreign threads only as a
// weak_ptr: a successful lock() on a foreign thread gives safe access to the
// counters, never to the process itself. A successful lock() does NOT prove
// the process is alive, because a foreign thread may be the one keeping the
// state referenced; owner-thread code must check process_alive() instead.
struct OutboundState {
    std::atomic<size_t> queued_bytes{0};
    std::atomic<size_t> hwm_override{0};        // 0 = use global default
    std::atomic<bool> slow_consumer{false};     // latched once the HWM trips
    std::atomic<bool> flush_posted{false};      // coalesces cross-thread flush requests
    std::atomic<bool> owner_alive{true};        // cleared by ~base_data_process

    size_t limit() const {
        size_t v = hwm_override.load(std::memory_order_relaxed);
        return v ? v : outbound_high_water_mark();
    }

    // Reserve n bytes. A message is always admitted when nothing is queued,
    // so a single payload larger than the HWM cannot wedge an idle connection;
    // otherwise the queued total must stay within the limit. Fails permanently
    // once the connection has been flagged as a slow consumer.
    bool try_reserve(size_t n) {
        if (slow_consumer.load(std::memory_order_acquire)) return false;
        const size_t lim = limit();
        size_t cur = queued_bytes.load(std::memory_order_relaxed);
        for (;;) {
            if (cur != 0 && (cur + n > lim || cur + n < cur)) return false;
            if (queued_bytes.compare_exchange_weak(cur, cur + n, std::memory_order_acq_rel,
                                                   std::memory_order_relaxed)) {
                return true;
            }
        }
    }

    void release(size_t n) {
        size_t cur = queued_bytes.load(std::memory_order_relaxed);
        for (;;) {
            size_t next = cur >= n ? cur - n : 0;
            if (queued_bytes.compare_exchange_weak(cur, next, std::memory_order_acq_rel,
                                                   std::memory_order_relaxed)) {
                return;
            }
        }
    }

    // Returns true exactly once: for the caller that latched the slow flag.
    bool mark_slow_consumer() {
        return !slow_consumer.exchange(true, std::memory_order_acq_rel);
    }
};

// Owner-thread check that the process owning this state still exists.
inline bool process_alive(const std::weak_ptr<OutboundState>& token) {
    std::shared_ptr<OutboundState> st = token.lock();
    return st && st->owner_alive.load(std::memory_order_acquire);
}

void note_dropped(OutboundKind kind, size_t bytes);
void note_slow_consumer_closed(OutboundKind kind);

} // namespace detail
} // namespace myframe

#endif
