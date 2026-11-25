#ifndef __WEB_SOCKET_DATA_PROCESS_H_
#define __WEB_SOCKET_DATA_PROCESS_H_

#include "base_def.h"
#include "base_data_process.h"
#include "web_socket_msg.h"
#include "common_def.h"
#include <memory>
#include <mutex>

class web_socket_process;
class web_socket_data_process;

// Thread-safe handle to a WebSocket connection's send path. It can be stored
// and used from any thread: the raw process pointer is only dereferenced on
// the connection's owner thread, after the connection generation and the
// process liveness token have both been verified.
struct ws_send_handle
{
    ObjId id;
    uint32_t generation{0};
    web_socket_data_process * proc{nullptr};   // owner-thread use only
    std::weak_ptr<myframe::detail::OutboundState> state;

    bool valid() const { return proc != nullptr && generation != 0; }
    bool expired() const { return state.expired(); }
};

namespace myframe { class WsContextImpl; }

class web_socket_data_process:public base_data_process
{
    friend class myframe::WsContextImpl;

    public:
        web_socket_data_process(web_socket_process *p);

        virtual ~web_socket_data_process();

        virtual void on_handshake_ok();

        virtual void on_close() {}

        virtual void on_ping(const char op_code, const std::string &ping_data);

        virtual uint64_t get_timeout_len();

        virtual uint64_t get_next_send_len(int8_t &content_type);
        // Whether there's at least one queued outbound message. Used by the
        // send pipeline to distinguish "no message" from "message with empty
        // payload" (since the latter has get_next_send_len == 0).
        bool has_pending_send() const {
            std::lock_guard<std::mutex> lock(_send_list_mutex);
            return !_send_list.empty();
        }
        virtual std::string *get_send_buf();
        virtual void msg_recv_finish() = 0;

        virtual void peer_close() override;

        virtual size_t process_recv_buf(const char *buf, size_t len);

        const std::string& recent_msg() const { return _recent_msg; }

        // Handle for cross-thread delivery (call on the owner thread, e.g.
        // from on_handshake_ok()).
        ws_send_handle send_handle();

        // Queue payload for the connection behind `h` from any thread. The
        // bytes are reserved against the target's high-water mark before the
        // message is posted; a slow consumer is closed and the payload dropped.
        // Returns false when the payload was not accepted.
        static bool post_send(const ws_send_handle & h,
                              std::shared_ptr<const std::string> payload,
                              int8_t con_type);

    protected:
        // Thread-safe: on the owner thread the frame is queued and sent
        // directly; from a foreign thread it is queued and a coalesced flush is
        // posted to the owner thread.
        void put_send_msg(ws_msg_type msg);

        // Owner thread only: queue a message whose bytes were already reserved.
        void enqueue_reserved(ws_msg_type msg, size_t reserved);

        void request_owner_flush();

        ws_msg_type get_send_msg();

    protected:
        web_socket_process *_process;
        ws_msg_type _p_current_send;
        std::list<ws_msg_type> _send_list;
        mutable std::mutex _send_list_mutex;
        std::string _recent_msg;
};


#endif
