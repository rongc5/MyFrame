#include "base_def.h"
#include "base_net_obj.h"
#include "base_singleton.h"
#include "http_res_process.h"
#include "common_exception.h"
#include "common_def.h"

#include "http_base_process.h"
#include "http_base_data_process.h"
#include "common_util.h"


http_res_process::http_res_process(std::shared_ptr<base_net_obj>  p):http_base_process(p)
{
    change_http_status(RECV_HEAD);
    _recv_body_length = 0;
    _recv_boundary_status = BOUNDARY_RECV_HEAD;
    _chunked = false;
    _chunk_state = CHUNK_SIZE;
    _cur_chunk_len = -1;
    _chunked_total = 0;
}

http_res_process::~http_res_process()
{		
}


void http_res_process::reset()
{
    http_base_process::reset();
    change_http_status(RECV_HEAD);

    _boundary_para.init();
    _recv_body_length = 0;
    _recv_boundary_status = BOUNDARY_RECV_HEAD;
    _chunked = false;
    _chunk_state = CHUNK_SIZE;
    _chunk_buf.clear();
    _cur_chunk_len = -1;
    _chunked_total = 0;
}


size_t http_res_process::process_recv_body(const char *buf, size_t len, int &result)
{
    result = 0;
    size_t ret = 0;
    if (_chunked)   // chunked 优先于方法判断：即便 GET 带 chunked 也须按帧消费,否则连接失步
    {
        return process_chunked(buf, len, result);
    }
    // Message framing is independent of the method. A bodyless GET/HEAD must
    // consume zero body bytes; the remaining buffer can contain the next request.
    {
        if (_boundary_para._boundary_str.length() == 0)
        {
            std::string *tmp_str = _req_head_para.get_header("Content-Length");
            if (!tmp_str)
            {
                // No Content-Length header: finish immediately to avoid hanging.
                // Do not consume current bytes here; they may belong to next request.
                result = 1;
                ret = 0;
            }
            else
            {
                uint64_t content_length = strtoull(tmp_str->c_str(), 0, 10);
                if (_recv_body_length >= content_length)
                {
                    // Content-Length: 0 or body already complete.
                    // Leave current bytes for next request in keep-alive/pipeline.
                    result = 1;
                    ret = 0;
                }
                else
                {
                    size_t remain = static_cast<size_t>(content_length - _recv_body_length);
                    size_t take = len < remain ? len : remain;
                    // The data callback reports whether it has handled this chunk,
                    // not whether the HTTP message is complete. A header-only read
                    // can call it with take=0 and result=1.
                    int data_result = 0;
                    ret = _data_process->process_recv_body(buf, take, data_result);
                    _recv_body_length += ret;
                }
                result = _recv_body_length >= content_length ? 1 : 0;
            }				
        }
        else //parse boundary
        {
            ret = get_boundary(buf, len, result);
        }
    }
    return ret;
}


void http_res_process::parse_first_line(const std::string & line)
{
    std::vector<std::string> tmp_vec;
    SplitString(line.c_str(), " ", &tmp_vec, SPLIT_MODE_ALL);
    if (tmp_vec.size() != 3) {
        THROW_COMMON_EXCEPT("http first line");
    }
    _req_head_para._method = tmp_vec[0];
    _req_head_para._version = tmp_vec[2];
    _req_head_para._url_path = tmp_vec[1];
}

void http_res_process::parse_header(std::string & recv_head)
{
    std::string &head_str = recv_head;
    std::vector<std::string> strList;
    SplitString(head_str.c_str(), CRLF, &strList, SPLIT_MODE_ALL);
    for (uint32_t i = 0; i < strList.size(); i++) {
        if (!i) {
            parse_first_line(strList[i]);
        }else {
            std::vector<std::string> tmp_vec;
            SplitString(strList[i].c_str(), ":", &tmp_vec, SPLIT_MODE_ONE);
            if (2 == tmp_vec.size()) {
                // 去掉冒号后值的前后 OWS（RFC7230：Name: OWS value OWS），否则 "Authorization: Bearer x"
                // 会带前导空格 " Bearer x"，消费方 rfind("Bearer ",0) 匹配失败。
                std::string k = tmp_vec[0], v = tmp_vec[1];
                size_t a = v.find_first_not_of(" \t");
                size_t b = v.find_last_not_of(" \t");
                v = (a == std::string::npos) ? std::string() : v.substr(a, b - a + 1);
                size_t kb = k.find_last_not_of(" \t");
                if (kb != std::string::npos) k = k.substr(0, kb + 1);
                _req_head_para._headers.insert(make_pair(k, v));
                PDEBUG("%s: %s", k.c_str(), v.c_str());
            }
        }
    }

    std::string * cookie_str = _req_head_para.get_header("Cookie");
    if (cookie_str)
    {
        std::vector<std::string> cookie_vec;
        SplitString(cookie_str->c_str(), ";", &cookie_vec, SPLIT_MODE_ALL);
        size_t c_num = cookie_vec.size();
        for (size_t ii = 0; ii < c_num; ii++)
        {
            std::vector<std::string> c_tmp_vec;
            SplitString(cookie_vec[ii].c_str(), "=", &c_tmp_vec, SPLIT_MODE_ONE);
            if (c_tmp_vec.size() == 2)
            {
                StringTrim(c_tmp_vec[0]);
                StringTrim(c_tmp_vec[1]);
                _req_head_para._cookie_list.insert(make_pair(c_tmp_vec[0], c_tmp_vec[1]));
            }
        }
    }

    std::string *tmp_str = NULL;
    if (_req_head_para._method == "POST" || _req_head_para._method == "PUT")
    {
        //parse content_type
        tmp_str = _req_head_para.get_header("Content-Type");
        if (tmp_str)
        {
            if (strcasestr(tmp_str->c_str(), "multipart/form-data") != NULL)
            {
                GetCaseStringByLabel(*tmp_str, "boundary=", "", _boundary_para._boundary_str);
            }
        }
    }

    // Transfer-Encoding（RFC 9112 §6/§7）：
    //   - 仅接受单一 "chunked"，其余编码/编码链 → 拒绝（关闭连接）；
    //   - TE 与 Content-Length 并存 → 拒绝（请求走私经典入口，§6.3）。
    _chunked = false;
    std::string *te = _req_head_para.get_header("Transfer-Encoding");
    if (te)
    {
        std::string tev = *te;
        StringTrim(tev);
        for (size_t ti = 0; ti < tev.length(); ti++) tev[ti] = (char)tolower((unsigned char)tev[ti]);
        if (tev != "chunked")
        {
            THROW_COMMON_EXCEPT("unsupported transfer-encoding: " << tev.c_str());
        }
        if (_req_head_para.get_header("Content-Length"))
        {
            THROW_COMMON_EXCEPT("both transfer-encoding and content-length (smuggling guard)");
        }
        _chunked = true;
        _chunk_state = CHUNK_SIZE;
        _chunk_buf.clear();
        _cur_chunk_len = -1;
        _chunked_total = 0;
    }
}

// chunked 请求体解码：帧格式 = [size-hex[;ext]] CRLF data CRLF ... 0 CRLF [trailer 行...] CRLF。
// 返回本次 buf 被消费的字节数；终止符之后的字节留给调用方（keep-alive 下属于下一请求）。
// 解码出的数据按普通 body 喂给业务 data process；完成时 result=1。
size_t http_res_process::process_chunked(const char *buf, size_t len, int &result)
{
    result = 0;
    _chunk_buf.append(buf, len);
    for (;;)
    {
        if (_chunk_state == CHUNK_SIZE)
        {
            size_t pos = _chunk_buf.find(CRLF);
            if (pos == std::string::npos)
            {
                if (_chunk_buf.length() > MAX_CHUNK_LINE_LEN)
                    THROW_COMMON_EXCEPT("chunk size line too long");
                return len;   // 等更多数据（本次全部暂存）
            }
            std::string line = _chunk_buf.substr(0, pos);
            size_t semi = line.find(';');
            if (semi != std::string::npos) line.erase(semi);   // chunk 扩展忽略
            StringTrim(line);
            if (line.empty() || !isxdigit((unsigned char)line[0]))
                THROW_COMMON_EXCEPT("bad chunk size line");
            char *endp = NULL;
            unsigned long sz = strtoul(line.c_str(), &endp, 16);
            if (endp == NULL || *endp != '\0')
                THROW_COMMON_EXCEPT("bad chunk size line");
            if (_chunked_total + sz > MAX_CHUNKED_BODY_LEN)
                THROW_COMMON_EXCEPT("chunked body too large (" << (_chunked_total + sz) << ")");
            _chunk_buf.erase(0, pos + 2);
            if (sz == 0)
            {
                _chunk_state = CHUNK_TRAILER;
                continue;
            }
            _cur_chunk_len = (long)sz;
            _chunk_state = CHUNK_DATA;
        }
        else if (_chunk_state == CHUNK_DATA)
        {
            if (_chunk_buf.length() < (size_t)_cur_chunk_len + 2)
                return len;   // 等整块到齐（总量已受 MAX_CHUNKED_BODY_LEN 约束）
            if (_chunk_buf[(size_t)_cur_chunk_len] != '\r' || _chunk_buf[(size_t)_cur_chunk_len + 1] != '\n')
                THROW_COMMON_EXCEPT("chunk data not CRLF terminated");
            int ignore = 0;
            _data_process->process_recv_body(_chunk_buf.c_str(), (size_t)_cur_chunk_len, ignore);
            _chunked_total += (uint64_t)_cur_chunk_len;
            _chunk_buf.erase(0, (size_t)_cur_chunk_len + 2);
            _cur_chunk_len = -1;
            _chunk_state = CHUNK_SIZE;
        }
        else // CHUNK_TRAILER：逐行消费，空行即结束（trailer 内容忽略）
        {
            size_t pos = _chunk_buf.find(CRLF);
            if (pos == std::string::npos)
            {
                if (_chunk_buf.length() > MAX_CHUNK_LINE_LEN)
                    THROW_COMMON_EXCEPT("chunk trailer too long");
                return len;
            }
            bool body_end = (pos == 0);
            _chunk_buf.erase(0, pos + 2);
            if (body_end)
            {
                result = 1;
                // 终止符后的剩余字节属于下一请求：留在调用方缓冲（返回值只计已消费部分）。
                size_t leftover = _chunk_buf.length();
                _chunk_buf.clear();
                return (leftover <= len) ? (len - leftover) : 0;   // 流式顺序保证 leftover<=len；防御性下界
            }
        }
    }
}

void http_res_process::recv_finish()
{
    _data_process->msg_recv_finish();
    http_base_data_process* data_process = get_process();
    if (data_process && data_process->async_response_pending()) {
        PDEBUG("http_res_process waiting async response");
        return;
    }
    notify_send_ready();
}

void http_res_process::send_finish()
{
    // Check Connection header before reset() clears it
    bool should_close = false;
    std::string* conn_hdr = _res_head_para.get_header("Connection");
    if (conn_hdr && strcasecmp(conn_hdr->c_str(), "close") == 0) {
        should_close = true;
    }

    reset();

    if (should_close) {
        request_close_now();
    }
}


size_t http_res_process::get_boundary(const char *buf, size_t len, int &result)
{	
    uint64_t content_length = 0;
    std::string *tmp_str = _req_head_para.get_header("Content-Length");
    if (tmp_str) 
    {
        content_length = strtoull(tmp_str->c_str(), 0, 10);
    }

    if (!content_length)
    {
        THROW_COMMON_EXCEPT("get boundary but content_len not found");
    }
    size_t ret = len;
    size_t p_len = 0;
    result = 0;
    _recv_body_length += len;
    //����Ҫ�ҵ�һ��boundary
    if (_recv_boundary_status == BOUNDARY_RECV_HEAD)
    {				
        _recv_boundary_head.append(buf, len);
        size_t pos = _recv_boundary_head.find("\r\n\r\n");
        if (pos != std::string::npos)
        {					
            _boundary_para._boundary_content_length = content_length - (_boundary_para._boundary_str.length() + BOUNDARY_EXTRA_LEN)
                - (pos+4);	
            std::string left_str;
            if (_recv_body_length == content_length)
            {											
                left_str = _recv_boundary_head.substr(pos+4, _boundary_para._boundary_content_length);
            }
            else if (_recv_body_length >= content_length - (_boundary_para._boundary_str.length() + BOUNDARY_EXTRA_LEN)) 
            {
                left_str = _recv_boundary_head.substr(pos+4, _boundary_para._boundary_content_length);
                _recv_boundary_status = BOUNDARY_RECV_TAIL;
            }
            else
            {
                left_str = _recv_boundary_head.substr(pos+4);
                _recv_boundary_status = BOUNDARY_RECV_BODY;
            }

            if (left_str.length() > 0)
            {
                p_len = _data_process->process_recv_body(left_str.c_str(), left_str.length(), result);
                if (_recv_body_length == content_length)
                    result = 1;
                p_len = left_str.length() - p_len;
            }
            else //ʲôҲ����
            {
            }
        }
        else //��Ҫ������ͷ
        {
            if (_recv_boundary_head.length() >= MAX_HTTP_HEAD_LEN)
                THROW_COMMON_EXCEPT("http boundary head too long (" << _recv_boundary_head.length() << ")");
        }
    }
    else if (_recv_boundary_status == BOUNDARY_RECV_BODY)//recv_body
    {		
        int tmp_len = len;
        if (_recv_body_length == content_length)
        {
            tmp_len = len - (_boundary_para._boundary_str.length() + BOUNDARY_EXTRA_LEN);

        }
        else if (_recv_body_length >= content_length - (_boundary_para._boundary_str.length() + BOUNDARY_EXTRA_LEN))
        {
            tmp_len = len - (_recv_body_length - (content_length - (_boundary_para._boundary_str.length() + BOUNDARY_EXTRA_LEN)));
            _recv_boundary_status = BOUNDARY_RECV_TAIL;
        }
        else //��Ҫ��body
        {
        }

        p_len = _data_process->process_recv_body(buf, tmp_len, result);
        p_len = tmp_len - p_len;

        if (_recv_body_length == content_length)
            result = 1; //������
    }
    else //recv tail
    {	
        //ʲôҲ���ɣ�ֻ������
        if (_recv_body_length == content_length)
            result = 1;				
    }
    _recv_body_length = _recv_body_length - p_len;
    return ret - p_len;
}
