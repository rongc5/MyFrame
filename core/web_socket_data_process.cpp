#include "web_socket_data_process.h"
#include "web_socket_msg.h"
#include "web_socket_process.h"

#include "base_net_obj.h"



web_socket_data_process::web_socket_data_process(web_socket_process *p):base_data_process(p->get_base_net())
{
    _process = p;
    PDEBUG("%p", this);
}

web_socket_data_process::~web_socket_data_process()
{
    PDEBUG("%p", this);
    for(std::list<ws_msg_type>::iterator itr = _send_list.begin(); itr != _send_list.end(); ++itr)
    {
        delete itr->_p_msg;
    }	
}

void web_socket_data_process::on_handshake_ok()		
{
    PDEBUG("%p", this);
}

void web_socket_data_process::on_ping(const char op_code, const std::string &ping_data)
{
    PDEBUG("%p", this);
}

void web_socket_data_process::peer_close()
{
    on_close();  // 调用子类可重写的回调
    base_data_process::peer_close();  // 调用基类实现
}

uint64_t web_socket_data_process::get_timeout_len()
{
    return WS_CONNECT_TIMEOUT;
}

std::string *web_socket_data_process::get_send_buf()
{	
    std::string *p_ret = NULL;
    ws_msg_type tmp = get_send_msg();
    p_ret = tmp._p_msg;
    return p_ret;
}

uint64_t web_socket_data_process::get_next_send_len(int8_t &content_type)
{
    uint64_t len = 0;
    if (_send_list.begin() != _send_list.end())
    {
        len = _send_list.begin()->_p_msg->length();
        content_type = _send_list.begin()->_con_type;
    }
    return len;
}

void web_socket_data_process::put_send_msg(ws_msg_type msg)
{
    _send_list.push_back(msg);
    // 仅在握手完成后触发发送，避免在握手阶段写入帧
    _process->notice_send();
}

ws_msg_type web_socket_data_process::get_send_msg()
{
    ws_msg_type ret;
    if (_send_list.begin() != _send_list.end())
    {
        ret = _send_list.front();
        _send_list.pop_front();
    }
    return ret;
}

size_t web_socket_data_process::process_recv_buf(const char *buf, size_t len)
{
    _recent_msg.append(buf, len); 

    return len;
}
