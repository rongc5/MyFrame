#pragma once

#include <memory>
#include <string>
#include "listen_connect.h"
#include "listen_process.h"
#include "../include/multi_protocol_factory.h"
#include "base_net_thread.h"

// 监听对象工厂：将 listen_thread::init 里的组装流程收敛到工厂
struct ListenFactory {
    static void init_listen(base_net_thread* owner_thread,
                            const std::string& ip, unsigned short port,
                            std::shared_ptr< listen_connect<listen_process> >& out_conn,
                            listen_process*& out_proc)
    {
        out_conn.reset(new listen_connect<listen_process>(ip, port));
        out_proc = new listen_process(out_conn);
        out_proc->set_listen_thread(owner_thread);
        out_conn->set_process(out_proc);
        out_conn->set_net_container(owner_thread->get_net_container());
    }
};
