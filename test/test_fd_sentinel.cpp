// P0 unit checks:
//   * fd 0 is a valid socket (registered on epoll, readable, closed exactly
//     once) and a closed connection uses the -1 sentinel, so it never writes
//     to - or closes - a descriptor number that was recycled afterwards.
//   * base_connect::destroy() is idempotent.
//   * OutboundState reservation semantics and the stream-queue high-water
//     mark / counters of base_data_process.
#include "base_connect.h"
#include "base_data_process.h"
#include "common_obj_container.h"
#include "outbound_guard.h"
#include "string_pool.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <memory>
#include <string>

namespace {

int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

class recording_process : public base_data_process {
public:
    explicit recording_process(std::shared_ptr<base_net_obj> p) : base_data_process(p) {}
    size_t process_recv_buf(const char* buf, size_t len) override {
        received.append(buf, len);
        return len;
    }
    void destroy() override {
        ++destroy_calls;
        base_data_process::destroy();
    }
    std::string received;
    int destroy_calls{0};
};

typedef base_connect<recording_process> test_conn;

bool fd_is_open(int fd) {
    errno = 0;
    return ::fcntl(fd, F_GETFD) != -1;
}

// Make a socketpair whose first end is descriptor 0.
bool socketpair_on_fd0(int& peer) {
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return false;
    if (fds[0] != 0) {
        if (::dup2(fds[0], 0) != 0) return false;
        ::close(fds[0]);
    }
    peer = fds[1];
    ::fcntl(0, F_SETFL, ::fcntl(0, F_GETFL) | O_NONBLOCK);
    return true;
}

size_t pending_bytes(int fd) {
    char buf[4096];
    ssize_t n = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT | MSG_PEEK);
    return n > 0 ? static_cast<size_t>(n) : 0;
}

void test_default_sentinel() {
    test_conn conn;  // no socket
    CHECK(conn.get_sfd() == -1);
    CHECK(base_net_obj::kInvalidFd == -1);
    CHECK(!conn.has_valid_fd());
}

void test_fd_zero_lifecycle() {
    ::close(0);  // free descriptor 0 (stdin is not used by this test)
    int peer = -1;
    CHECK(socketpair_on_fd0(peer));

    common_obj_container container(77);
    std::shared_ptr<test_conn> conn(new test_conn(0));
    conn->set_process(new recording_process(conn));
    conn->set_net_container(&container);
    CHECK(conn->get_sfd() == 0);
    CHECK(conn->has_valid_fd());

    // fd 0 must be registered on epoll like any other socket.
    CHECK(::send(peer, "ping", 4, 0) == 4);
    for (int i = 0; i < 20 && conn->process()->received.size() < 4; ++i) container.obj_process();
    CHECK(conn->process()->received == "ping");

    // Owner-thread send on fd 0 works.
    conn->process()->put_send_copy("pong");
    for (int i = 0; i < 5 && pending_bytes(peer) < 4; ++i) container.obj_process();
    CHECK(pending_bytes(peer) == 4);
    char drain[16];
    (void)::recv(peer, drain, sizeof(drain), 0);

    // destroy(): closes fd 0 exactly once and switches to the -1 sentinel.
    conn->destroy();
    CHECK(conn->get_sfd() == -1);
    CHECK(!fd_is_open(0));
    CHECK(conn->process()->destroy_calls == 1);
    conn->destroy();  // idempotent
    CHECK(conn->process()->destroy_calls == 1);

    // Recycle descriptor 0 for an unrelated socket.
    int peer2 = -1;
    CHECK(socketpair_on_fd0(peer2));
    CHECK(fd_is_open(0));

    // The closed connection must not write to the recycled descriptor.
    conn->process()->put_send_copy("stale-bytes");
    conn->notice_send();
    container.obj_process();
    CHECK(pending_bytes(peer2) == 0);

    // Destroying the stale object must not close the recycled fd 0.
    container.erase(conn->get_id()._id);
    conn.reset();
    CHECK(fd_is_open(0));

    ::close(0);
    ::close(peer2);
    ::close(peer);
}

void test_outbound_state() {
    myframe::detail::OutboundState st;
    st.hwm_override = 100;
    CHECK(st.limit() == 100);
    CHECK(st.try_reserve(250));        // empty queue always admits one message
    CHECK(!st.try_reserve(1));         // over the limit now
    st.release(250);
    CHECK(st.queued_bytes.load() == 0);
    CHECK(st.try_reserve(60));
    CHECK(st.try_reserve(40));         // exactly at the limit
    CHECK(!st.try_reserve(1));
    st.release(1000);                  // never underflows
    CHECK(st.queued_bytes.load() == 0);
    CHECK(st.mark_slow_consumer());
    CHECK(!st.mark_slow_consumer());   // latched exactly once
    CHECK(!st.try_reserve(1));         // slow consumers admit nothing
}

void test_stream_queue_hwm() {
    myframe::reset_outbound_stats();
    // Unattached process: owner-thread path with accounting only.
    recording_process proc{std::shared_ptr<base_net_obj>()};
    proc.set_outbound_high_water_mark(1000);
    CHECK(proc.outbound_high_water_mark() == 1000);
    proc.put_send_copy(std::string(600, 'a'));
    proc.put_send_copy(std::string(400, 'b'));
    CHECK(proc.outbound_queued_bytes() == 1000);
    proc.put_send_copy(std::string(10, 'c'));  // exceeds the HWM -> dropped
    CHECK(proc.outbound_queued_bytes() == 1000);
    CHECK(proc.is_slow_consumer());
    myframe::OutboundStats st = myframe::outbound_stats();
    CHECK(st.stream_dropped_messages == 1);
    CHECK(st.stream_dropped_bytes == 10);
    CHECK(st.stream_slow_consumer_closed == 1);
    std::string* p = proc.get_send_buf();
    CHECK(p && p->size() == 600);
    CHECK(proc.outbound_queued_bytes() == 400);
    myframe::string_release(p);
    proc.reset();  // clears the queue and releases the reservation
    CHECK(proc.outbound_queued_bytes() == 0);
}

} // namespace

int main() {
    test_default_sentinel();
    test_fd_zero_lifecycle();
    test_outbound_state();
    test_stream_queue_hwm();
    if (g_fail) {
        std::fprintf(stderr, "test_fd_sentinel: %d FAIL\n", g_fail);
        return 1;
    }
    std::printf("test_fd_sentinel: ALL PASS\n");
    return 0;
}
