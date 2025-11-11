// MyFrame Unified Protocol - WebSocket Client Test
// 用于测试 unified_mixed_server 的 WebSocket 功能

#include "../core/base_net_thread.h"
#include "../core/out_connect.h"
#include "../core/web_socket_req_process.h"
#include "../core/web_socket_data_process.h"
#include "../core/string_pool.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <signal.h>

using namespace std;

// 简单的 WebSocket 客户端数据处理器
class SimpleWsClientDataProcess : public web_socket_data_process {
public:
    SimpleWsClientDataProcess(web_socket_process* p)
        : web_socket_data_process(p)
        , _msg_count(0)
    {
    }

    virtual void on_handshake_ok() override {
        cout << "[Client] ✅ WebSocket handshake completed!" << endl;

        // 发送测试消息
        send_text_message("Hello from client!");
    }

    virtual void msg_recv_finish() override {
        auto& frame_header = _process->get_recent_recv_frame_header();

        cout << "[Client] 📩 Received message: opcode=0x"
             << hex << (int)frame_header._op_code << dec
             << ", payload=" << _recent_msg << endl;

        _msg_count++;

        if (_msg_count == 1) {
            // 发送第二条消息
            send_text_message("Second message");
        }
        else if (_msg_count == 2) {
            // 发送第三条消息
            send_text_message("Third and final message");
        }
        else if (_msg_count >= 3) {
            // 收到3条回复后关闭
            cout << "[Client] ✅ All messages echoed back successfully!" << endl;
            cout << "[Client] Closing connection..." << endl;

            // 发送关闭帧
            ws_msg_type close_msg;
            close_msg._p_msg = myframe::string_acquire();
            close_msg._p_msg->assign("Goodbye");
            close_msg._con_type = 0x8; // CLOSE
            put_send_msg(close_msg);
            _process->notice_send();
        }
    }

    virtual void on_close() override {
        cout << "[Client] Connection closed" << endl;
    }

private:
    void send_text_message(const string& text) {
        ws_msg_type msg;
        msg._p_msg = myframe::string_acquire();
        msg._p_msg->assign(text);
        msg._con_type = 0x1; // TEXT
        put_send_msg(msg);
        _process->notice_send();

        cout << "[Client] 📤 Sent: " << text << endl;
    }

    int _msg_count;
};


int main(int argc, char** argv) {
    string host = "127.0.0.1";
    unsigned short port = 8080;
    string path = "/ws";

    if (argc > 1) host = argv[1];
    if (argc > 2) port = (unsigned short)atoi(argv[2]);
    if (argc > 3) path = argv[3];

    cout << "=== MyFrame WebSocket Client Test ===" << endl;
    cout << "Connecting to: ws://" << host << ":" << port << path << endl;

    try {
        // 创建网络线程
        base_net_thread th;
        th.start();

        // 创建 WebSocket 连接
        auto conn = make_shared< out_connect<web_socket_req_process> >(host, port);

        // 创建 WebSocket 请求处理器
        auto proc = new web_socket_req_process(conn);

        // 设置请求参数
        proc->get_req_para()._s_path = path;
        proc->get_req_para()._s_host = host + ":" + to_string(port);
        proc->get_req_para()._origin = "http://" + host + ":" + to_string(port);

        // 设置数据处理器
        proc->set_process(new SimpleWsClientDataProcess(proc));

        // 设置到连接
        conn->set_process(proc);
        conn->set_net_container(th.get_net_container());

        // 连接
        cout << "[Client] Connecting..." << endl;
        conn->connect();

        cout << "[Client] ✅ Connection initiated" << endl;

        // 设置超时（10秒后自动退出）
        thread timeout_thread([&th]() {
            this_thread::sleep_for(chrono::seconds(10));
            cout << "[Client] Timeout after 10 seconds" << endl;
            base_thread::stop_all_thread();
        });
        timeout_thread.detach();

        // 等待网络线程结束
        th.join_thread();

        cout << "[Client] Test completed" << endl;
        return 0;

    } catch (const exception& e) {
        cerr << "[Client] ❌ Error: " << e.what() << endl;
        return 1;
    }
}
