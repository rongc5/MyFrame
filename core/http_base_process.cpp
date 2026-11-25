#include "http_base_process.h"
#include "base_connect.h"
#include "common_exception.h"
#include "http_base_data_process.h"

#include <algorithm>


http_base_process::http_base_process(std::shared_ptr<base_net_obj> p):base_data_process(p)
{
    _data_process = NULL;
}

http_base_process::~http_base_process()
{
    if (_data_process != NULL)
        delete _data_process;
}

http_req_head_para &http_base_process::get_req_head_para()
{
    return _req_head_para;
}

http_res_head_para &http_base_process::get_res_head_para()
{
    return _res_head_para;
}

void http_base_process::reset()
{
    _req_head_para.init();
    _res_head_para.init();
}

void http_base_process::set_process(http_base_data_process * data_process)
{
    if (_data_process != NULL && _data_process != data_process) {
        delete _data_process;
        _data_process = NULL;
    }
    _data_process = data_process;
}

size_t http_base_process::process_recv_buf(const char *buf, size_t len)
{
    if (_http_status > RECV_BODY)
    {
        THROW_COMMON_EXCEPT("http recv status not correct " << _http_status);
    }

    size_t ret = 0;
    size_t head_len = 0;
    bool status_change = false;
    // Network buffers are length-delimited and are not guaranteed to be NUL
    // terminated. check_head_finish() performs a bounded, copy-free search so
    // fragmented headers cannot be completed by stale bytes beyond `len`.
    if (_http_status == RECV_HEAD)
    {
        if (!check_head_finish(buf, len, head_len))
        {
            return 0;  // header still incomplete: keep accumulating
        }
        status_change = true;
    }

    if (_http_status == RECV_BODY)
    {
        int result = 0;
        if (status_change)
        {
            ret = head_len;
            ret += process_recv_body(buf + head_len, len - head_len, result);
        }
        else
        {
            ret = process_recv_body(buf, len, result);
        }

        if (result == 1)
        {
            recv_finish();
        }
    }
    return ret;
}

std::string* http_base_process::get_send_buf()
{
    if (_http_status != SEND_HEAD && _http_status != SEND_BODY)
    {
        return NULL;
    }

    std::string *ret_str = NULL;
    if (_http_status == SEND_HEAD)
    {
        ret_str = _data_process->get_send_head();

        if (!ret_str)
            return NULL;

        _http_status = SEND_BODY;
        return ret_str;
    }
    else if (_http_status == SEND_BODY)
    {
        int result = 0;
        ret_str = _data_process->get_send_body(result);
        if (result == 1)
            send_finish();
    }

    return ret_str;
}

void http_base_process::handle_msg(std::shared_ptr<normal_msg> & p_msg)
{
    _data_process->handle_msg(p_msg);
    return;
}

void http_base_process::handle_timeout(std::shared_ptr<timer_msg> & t_msg)
{
    _data_process->handle_timeout(t_msg);
}

void http_base_process::destroy()
{
    _data_process->destroy();
}

void http_base_process::peer_close()
{
    if (_data_process) _data_process->notify_peer_close();
}

/**************************** Internal helpers ****************************/

void http_base_process::change_http_status(HTTP_STATUS status, bool if_change_send)
{
    auto sp = _p_connect.lock();
    _http_status = status;
    if (status == SEND_HEAD && if_change_send && sp)
    {
        sp->notice_send();
    }
}

void http_base_process::notify_send_ready()
{
    change_http_status(SEND_HEAD);
}

http_base_data_process * http_base_process::get_process()
{
    return _data_process;
}

bool http_base_process::check_head_finish(const char *buf, size_t len, size_t &head_len)
{
    static const char kCrlf2[] = CRLF2;
    const size_t kCrlf2Len = sizeof(kCrlf2) - 1;
    head_len = 0;

    const char *end = buf + len;
    const char *hit = (buf && len >= kCrlf2Len)
                          ? std::search(buf, end, kCrlf2, kCrlf2 + kCrlf2Len)
                          : end;
    if (hit == end)
    {
        // No terminator yet: reject as soon as the partial header exceeds the
        // limit instead of buffering an unbounded header block.
        if (len > MAX_HTTP_HEAD_LEN)
        {
            THROW_COMMON_EXCEPT("http head too long (" << len << ")");
        }
        return false;
    }

    const size_t pos = static_cast<size_t>(hit - buf);
    if (pos + kCrlf2Len > MAX_HTTP_HEAD_LEN)
    {
        THROW_COMMON_EXCEPT("http head too long (" << (pos + kCrlf2Len) << ")");
    }

    head_len = pos + kCrlf2Len;
    std::string recv_head(buf, head_len);
    change_http_status(RECV_BODY);
    parse_header(recv_head);

    _data_process->header_recv_finish();
    return true;
}
