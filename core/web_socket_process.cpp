#include "web_socket_data_process.h"
#include "web_socket_process.h"
#include "base_net_obj.h"
#include "string_pool.h"

#include "common_exception.h"
#include "mybase64.h"
#include "common_util.h"


web_socket_process::web_socket_process(std::shared_ptr<base_net_obj> p):base_data_process(p)
{
    _wb_status = WB_INIT_STAUTS;
    _if_send_mask = true;
    _require_recv_mask = false;
    _p_data_process = NULL;
}

const size_t web_socket_process::kMaxPendingControlFrames;

web_socket_process::~web_socket_process()
{
    if (_p_data_process != NULL)
        delete _p_data_process;

    for (std::list<std::string*>::iterator itr = _p_tmp_str.begin(); itr != _p_tmp_str.end(); ++itr)
    {
        if (*itr != NULL)
            delete *itr;
    }
}


void web_socket_process::set_process(web_socket_data_process * data_process)
{
    if (_p_data_process && _p_data_process != data_process) {
        delete _p_data_process;
        _p_data_process = NULL;
    }

    _p_data_process = data_process;
}

//յ
size_t web_socket_process::process_recv_buf(const char *buf, const size_t len)
{
    if (WB_INIT_STAUTS == _wb_status)
    {
        RECV_WB_INIT_STAUTS_PROCESS(buf, len);
    }
    else if (WB_HANDSHAKE_OK == _wb_status)
    {
        RECV_WB_HANDSHAKE_OK_PROCESS(buf, len);
    }
    else if (WB_HEAD_FINISH == _wb_status)
    {
        RECV_WB_HEAD_FINISH_PROCESS(buf, len);
    }
    else
    {			
        THROW_COMMON_EXCEPT("WB_HANDSHAKE_FAIL status, can't recv and send data");
    }
    return len;
}

void web_socket_process::handle_timeout(std::shared_ptr<timer_msg> & t_msg)
{
    if (t_msg->_timer_type == WEB_SOCKET_HANDSHAKE_OK_TIMER_TYPE)
    {
        if (_wb_status != WB_HANDSHAKE_OK)
        {
            THROW_COMMON_EXCEPT("the web socket handshake time out, delete it");
        }
        else
        {
            PDEBUG("web socket handshake timer arrive, status ok");
        }

        return ;
    }

    // 2026-08-02: 连接销毁期间残留定时器仍会派发到这里, _p_data_process 可能已释放
    // (对端在 WS 握手完成后立即关闭时触发, SIGSEGV backtrace: base_connect::destroy
    //  -> handle_timeout). 与 handle_msg 同样判空防护。
    if (!_p_data_process) {
        return ;
    }

    _p_data_process->handle_timeout(t_msg);
}

void web_socket_process::handle_msg(std::shared_ptr<normal_msg> & p_msg)
{
    if (!_p_data_process) {
        return ;
    }

    _p_data_process->handle_msg(p_msg);
    return ;
}

std::string* web_socket_process::get_send_buf()
{
    std::string *p_str = NULL;
    if (WB_HEAD_FINISH == _wb_status) //ͷ
    {								
        p_str = SEND_WB_HEAD_FINISH_PROCESS();
    }
    else if (WB_HANDSHAKE_OK == _wb_status) // 
    {
        p_str = SEND_WB_HANDSHAKE_OK_PROCESS();
    }
    else if (WB_INIT_STAUTS == _wb_status)//ڵ״̬
    {
        p_str = SEND_WB_INIT_STAUTS_PROCESS();
    }
    else//WB_HANDSHAKE_FAIL
    {
        THROW_COMMON_EXCEPT("WB_HANDSHAKE_FAIL status, can't recv and send data");
    }
    return p_str;
}


void web_socket_process::peer_close()
{
    if (_p_data_process != NULL)
        _p_data_process->notify_peer_close();
}

void web_socket_process::destroy()
{
    if (_p_data_process != NULL)
        _p_data_process->destroy();
}

void web_socket_process::send_ping(const char op_code, const std::string &ping_data)
{
    if (!on_owner_thread()) {
        // _p_tmp_str and the frame state belong to the owner thread.
        std::string data = ping_data;
        post_to_owner_thread([this, op_code, data]() { send_ping(op_code, data); });
        return;
    }
    if (ping_data.length() <= 125) // control frame payloads are limited to 125 bytes
    {
        if (_p_tmp_str.size() >= kMaxPendingControlFrames)
        {
            // The peer does not read its PONGs: drop the frame and close the
            // connection instead of queueing without bound.
            PDEBUG("websocket pending control frames over limit, closing");
            request_close_now();
            return;
        }
        std::string *p_str = new std::string;
        *p_str = make_control_frame(op_code, ping_data);
        _p_tmp_str.push_back(p_str);
        notice_send();
    }
}

void web_socket_process::send_close(const std::string &close_data)
{
    if (!on_owner_thread()) {
        std::string data = close_data;
        post_to_owner_thread([this, data]() { send_close(data); });
        return;
    }
    if (_p_tmp_str.size() >= kMaxPendingControlFrames)
    {
        PDEBUG("websocket pending control frames over limit, closing");
        request_close_now();
        return;
    }
    std::string payload = close_data;
    if (payload.length() > 125)
    {
        payload.resize(125);
    }
    std::string *p_str = new std::string;
    *p_str = make_control_frame(0x08, payload);
    _p_tmp_str.push_back(p_str);
    notice_send();
}

std::string web_socket_process::make_control_frame(const char op_code, const std::string &payload)
{
    std::string mask_key;
    if (_if_send_mask)
    {
        uint32_t r = static_cast<uint32_t>(rand()) ^ static_cast<uint32_t>(GetMilliSecond());
        mask_key.assign(reinterpret_cast<char*>(&r), 4);
    }
    return web_socket_frame_header::gen_control_frame(op_code, payload, mask_key);
}
/************************************************************/
std::string web_socket_process::get_web_accept_key(const std::string &ws_key)
{
    std::string accept_key;
    std::string tmp_key = ws_key + WEB_SOCKET_GUID;			
    unsigned char md[20];
    SHA1((unsigned char*)tmp_key.c_str(), tmp_key.length(), md);
    std::string aa = std::string((char*)md, 20);
    accept_key = CBase64::encode(aa);
    return accept_key;
}


web_socket_frame_header &web_socket_process::get_recent_recv_frame_header()
{
    return _recent_recv_web_header;
}

web_socket_frame_header &web_socket_process::get_recent_send_frame_header()
{
    return _recent_send_web_header;
}

void web_socket_process::notice_send()
{
    auto sp = _p_connect.lock();
    if (!sp) return;
    if (!sp->on_owner_thread()) {
        // _wb_status is owner-thread state; re-check it there.
        post_to_owner_thread([this]() { notice_send(); });
        return;
    }
    if (_wb_status == WB_HANDSHAKE_OK)
        sp->notice_send();
}

const std::string &web_socket_process::get_recv_header()
{
    return _recv_header;
}

const std::string &web_socket_process::get_send_header()
{
    return _send_header;
}		

bool web_socket_process::check_head_finish()
{
    bool ret = false;
    size_t pos =  _recv_header.find("\r\n\r\n");
    if (pos != std::string::npos)
    {
        _recv_header.erase(pos + 4);
        parse_header();                     
        ret = true;
    }
    else
    {
        if (_recv_header.length() > MAX_HTTP_HEAD_LEN) //httpͷҪ10k
        {
            THROW_COMMON_EXCEPT("http head too long (" << _recv_header.length() << ")");
        }
    }			
    return ret;
}


std::string *web_socket_process::SEND_WB_HANDSHAKE_OK_PROCESS()
{
    std::string *p_str = NULL;
    // Mid-body data send: finish the current data frame before injecting any
    // queued control frame. Popping _p_tmp_str here would splice a pong/close
    // into the middle of a data frame's payload and corrupt the on-wire stream.
    if (_recent_send_web_header._wb_body_status == WB_FRAME_BODY_STAUS)
    {
        p_str = _p_data_process->get_send_buf();
        if (p_str != NULL)
        {
            if (_recent_send_web_header._mask_flag == 1)
            {
                *p_str = _recent_send_web_header.mask_code(p_str->c_str(), p_str->length());
            }
            _recent_send_web_header.update(p_str->length()); //change status
            if (_recent_send_web_header.if_finish())
            {
                _recent_send_web_header.clear();
            }
        }
    }
    else // WB_FRAME_HEAD_STAUS — frame boundary; safe to send control frames
    {
        if (_p_tmp_str.begin() != _p_tmp_str.end())
        {
            p_str = _p_tmp_str.front();
            _p_tmp_str.pop_front();
            PDEBUG("real send control frame to peer");
        }
        else if (_p_data_process->has_pending_send())
        {
            int8_t content_type = 0;
            uint64_t len = _p_data_process->get_next_send_len(content_type);
            p_str = new std::string();
            std::string mask_key;
            if (_if_send_mask)
            {
                int32_t r = rand();
                mask_key.assign((char*)&r, 4);
            }
            *p_str = _recent_send_web_header.gen_frame_header(len, mask_key, content_type); //change status
            if (len == 0)
            {
                // Empty-payload frame: header is the whole frame on the wire.
                // Consume the queued message and reset back to HEAD so the next
                // call doesn't enter the body branch with no body to send.
                std::string* empty_body = _p_data_process->get_send_buf();
                if (empty_body) myframe::string_release(empty_body);
                _recent_send_web_header.clear();
            }
        }
    }
    return p_str;
}


size_t web_socket_process::RECV_WB_HANDSHAKE_OK_PROCESS(const char *buf, const size_t len)
{
    PDEBUG("RECV_WB_HANDSHAKE_OK_PROCESS %d", len);
    char *left_buf = (char*)buf;
    uint32_t left_len = len;
    while(left_len > 0)
    {							       
        if (_recent_recv_web_header._wb_body_status == WB_FRAME_HEAD_STAUS)
        {
            _recent_recv_web_header.process(left_buf, left_len);
            if (_recent_recv_web_header._wb_body_status == WB_FRAME_BODY_STAUS &&
                _require_recv_mask && _recent_recv_web_header._mask_flag != 1)
            {
                // RFC 6455 5.1: fail the connection (1002 protocol error).
                THROW_COMMON_EXCEPT("websocket client frame must be masked");
            }
        }

        if (_recent_recv_web_header._wb_body_status == WB_FRAME_BODY_STAUS)
        {
            uint32_t tmp_left = _recent_recv_web_header.update(left_len);//״̬߱任
            int8_t tmp_code = _recent_recv_web_header._op_code;
            if (tmp_code != 0x08 && tmp_code != 0x09 && tmp_code != 0x0a) //control frames handled below
            {
                // Process available body bytes. The amount may be 0 — legitimate
                // for empty payload frames (incl. final empty CONTINUATION which
                // completes a fragmented message) or for headers received without
                // their body yet. if_finish() decides whether the frame is done.
                size_t chunk = left_len - tmp_left;
                if (_recent_recv_web_header._mask_flag == 1)
                {
                    std::string tmp_ret = _recent_recv_web_header.mask_code(left_buf, chunk);
                    _p_data_process->process_recv_buf(tmp_ret.c_str(), tmp_ret.length());
                }
                else
                {
                    _p_data_process->process_recv_buf(left_buf, chunk);
                }

                left_buf = left_buf + chunk;
                left_len = tmp_left;

                if (_recent_recv_web_header.if_finish())
                {
                    _p_data_process->msg_recv_finish();
                }
            }
            else //control frames
            {
                if (_recent_recv_web_header._mask_flag == 1)
                {
                    std::string tmp_ret = _recent_recv_web_header.mask_code(left_buf, left_len - tmp_left);
                    _ping_data.append(tmp_ret);
                }
                else
                {
                    _ping_data.append(left_buf,  left_len - tmp_left);
                }
                left_buf = left_buf + (left_len - tmp_left);
                left_len = tmp_left; 
                if(_recent_recv_web_header.if_finish()) //״̬,pingڵײ㴦
                {
                    if (tmp_code == 0x08)
                    {
                        send_close(_ping_data);
                        _p_data_process->notify_peer_close();
                        request_close_after(50);
                    }
                    else
                    {
                        _p_data_process->on_ping(tmp_code, _ping_data);
                    }
                    _ping_data.clear();
                }
            }
        }
    }
    return len;
}
