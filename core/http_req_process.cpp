#include "base_def.h"
#include "http_req_process.h"
#include "base_singleton.h"
#include "http_base_process.h"
#include "common_exception.h"
#include "common_def.h"

#include "http_base_data_process.h"
#include "common_util.h"
#include "protocol_limits.h"

http_req_process::http_req_process(std::shared_ptr<base_net_obj> p):http_base_process(p)
{
    http_base_process::change_http_status(SEND_HEAD);
    _cur_chunked_len = -1;
    _cur_chunked_rec_len = -1;
    _recv_body_length = 0;
    _recv_type = CONTENT_LENGTH_TYPE;
}

http_req_process::~http_req_process()
{
}

void http_req_process::reset()
{
    http_base_process::reset();
    http_base_process::change_http_status(SEND_HEAD, false);
    _req_head_para.init();
    _res_head_para.init();

    _cur_chunked_len = -1;
    _cur_chunked_rec_len = -1;
    _recv_body_length = 0;
    _chunked_body.clear();
}


void http_req_process::peer_close()
{
    if (OTHER_TYPE == _recv_type)
    {
        recv_finish();
    }
}

void http_req_process::parse_first_line(const std::string & line)
{
    // HTTP response: "HTTP/1.1 200 OK" or "HTTP/1.1 404 Not Found"
    // Status text may contain spaces, so we only split the first 2 spaces
    std::vector<std::string> tmp_vec;
    SplitString(line.c_str(), " ", &tmp_vec, SPLIT_MODE_ALL);
    if (tmp_vec.size() < 3) {
        THROW_COMMON_EXCEPT("http first line");
    }
    _res_head_para._response_code = strtoull(tmp_vec[1].c_str(), 0, 10);
    _res_head_para._version = tmp_vec[0];
    // Join remaining parts as status text (e.g. "Not Found")
    _res_head_para._response_str = tmp_vec[2];
    for (size_t i = 3; i < tmp_vec.size(); ++i) {
        _res_head_para._response_str += " " + tmp_vec[i];
    }
}

void http_req_process::parse_header(std::string & recv_head)
{
    PDEBUG("recv_head:%s", recv_head.c_str());
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
                // Conflicting duplicate Content-Length values (in any letter
                // case) make the response framing ambiguous: reject them.
                if (strcasecmp(tmp_vec[0].c_str(), "Content-Length") == 0) {
                    std::string* prev = _res_head_para.get_header("Content-Length");
                    if (prev) {
                        std::string pv = *prev, nv = tmp_vec[1];
                        StringTrim(pv);
                        StringTrim(nv);
                        if (pv != nv) {
                            THROW_COMMON_EXCEPT("conflicting content-length headers in http response");
                        }
                        continue;
                    }
                }
                _res_head_para._headers.insert(make_pair(tmp_vec[0], tmp_vec[1]));
                PDEBUG("%s: %s", tmp_vec[0].c_str(), tmp_vec[1].c_str());
            }
        }
    }

    //parse chunked
    std::string * tmp_str= _res_head_para.get_header("Transfer-Encoding");
    if (tmp_str)
    {	
        _res_head_para._chunked = *tmp_str;
        StringTrim(_res_head_para._chunked);	
    }

}

size_t http_req_process::process_recv_body(const char *buf, size_t len, int &result)
{
    result = 0;
    size_t ret  = 0;
    uint64_t content_length = 0;
    std::string *tmp_str = _res_head_para.get_header("Content-Length");
    if (tmp_str)
    {
        // Strict framing: the value (optional whitespace aside) must be
        // plain ASCII digits; negative or garbage lengths close the connection.
        std::string v = *tmp_str;
        size_t a = v.find_first_not_of(" \t");
        size_t b = v.find_last_not_of(" \t");
        v = (a == std::string::npos) ? std::string() : v.substr(a, b - a + 1);
        if (!myframe::parse_strict_decimal_u64(v, content_length))
        {
            THROW_COMMON_EXCEPT("invalid content-length value in http response");
        }
    }
    if (_res_head_para._chunked == "chunked")
    {
        _recv_type = CHUNK_TYPE;
        ret = get_chuncked(buf, len, result);				
    }
    else if (tmp_str)
    {
        _recv_type = CONTENT_LENGTH_TYPE;
        uint64_t remain = _recv_body_length >= content_length
            ? 0 : content_length - _recv_body_length;
        size_t take = static_cast<size_t>(std::min<uint64_t>(len, remain));
        int data_result = 0;
        if (take) ret = std::min(take, _data_process->process_recv_body(buf, take, data_result));
        _recv_body_length += ret;
        result = _recv_body_length >= content_length ? 1 : 0;
    }
    else
    {
        _recv_type = OTHER_TYPE;
        //THROW_COMMON_EXCEPT(-1, "recv body fail " << http_base_process<DATA_PROCESS>::_recv_head.c_str());
        ret = _data_process->process_recv_body(buf, len, result);
        _recv_body_length += ret;
    }
    return ret;
}

size_t http_req_process::get_chuncked(const char *buf, size_t len, int &result)
{
    size_t ret = len;
    size_t p_len = 0;
    result = 0;
    if (len == 0)
        return 0;

    _chunked_body.append(buf, len);
    while (1)
    {
        if (_cur_chunked_len == -1)
        {   
            std::string sTmp;
            int nRet= GetStringByLabel(_chunked_body, "", "\r\n", sTmp, 0, 1);            
            if (nRet != -1)
            {
                // Strict chunk-size: hex digits only (extensions stripped),
                // bounded so the signed length arithmetic below cannot wrap.
                std::string size_line = sTmp;
                size_t semi = size_line.find(';');
                if (semi != std::string::npos) size_line.erase(semi);
                StringTrim(size_line);
                uint64_t chunk_sz = 0;
                if (!myframe::parse_strict_hex_u64(size_line, chunk_sz) ||
                    chunk_sz > static_cast<uint64_t>(MAX_RECV_SIZE)) {
                    THROW_COMMON_EXCEPT("invalid chunk size in http response");
                }
                _cur_chunked_len = static_cast<int64_t>(chunk_sz);
                _cur_chunked_rec_len = nRet;
                if(_cur_chunked_len == 0)
                {         
                    result = 1; // last chunk
                    break;
                }

                if (_cur_chunked_len + 2 + _cur_chunked_rec_len  > (int)_chunked_body.length())
                {
                    break;
                }
                else
                {
                    p_len = _data_process->process_recv_body((char*)_chunked_body.substr(_cur_chunked_rec_len, _cur_chunked_len).c_str(), _cur_chunked_len, result);
                    p_len = _cur_chunked_len - p_len;

                    _chunked_body = _chunked_body.substr(_cur_chunked_rec_len+2+_cur_chunked_len);
                    _cur_chunked_len = -1;
                    if (0 == _chunked_body.length())
                        break;
                }
            }
            else
            {
                break;
            }
        }
        else // chunk length known, waiting for the rest of the chunk data
        {          
            if (_cur_chunked_len+2+_cur_chunked_rec_len > (int)_chunked_body.length())
            {
                break;
            }
            else
            {
                p_len = _data_process->process_recv_body((char*)_chunked_body.substr(_cur_chunked_rec_len, _cur_chunked_len).c_str(), _cur_chunked_len, result); 
                p_len = _cur_chunked_len - p_len;
                _chunked_body = _chunked_body.substr(_cur_chunked_len+_cur_chunked_rec_len+2);
                _cur_chunked_len = -1;
                if ( 0 == _chunked_body.length())
                    break;
            }
        }
    }
    return ret - p_len;
}

void http_req_process::recv_finish()
{
    // Check if server sent Connection: close before reset() clears headers
    bool should_close = false;
    std::string* conn_hdr = _res_head_para.get_header("Connection");
    if (conn_hdr && strcasecmp(conn_hdr->c_str(), "close") == 0) {
        should_close = true;
    }

    _data_process->msg_recv_finish();
    reset();

    if (should_close) {
        request_close_now();
    }
}

void http_req_process::send_finish()
{
    //http_base_process<DATA_PROCESS>::_http_status = RECV_HEAD;
    change_http_status(RECV_HEAD);
}
