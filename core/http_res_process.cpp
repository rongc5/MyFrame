#include "base_def.h"
#include "base_net_obj.h"
#include "base_singleton.h"
#include "http_res_process.h"
#include "common_exception.h"
#include "common_def.h"

#include "http_base_process.h"
#include "http_base_data_process.h"
#include "common_util.h"
#include "protocol_limits.h"


http_res_process::http_res_process(std::shared_ptr<base_net_obj>  p):http_base_process(p)
{
    change_http_status(RECV_HEAD);
    _recv_body_length = 0;
    _recv_boundary_status = BOUNDARY_RECV_HEAD;
    _has_content_length = false;
    _content_length = 0;
    _chunked = false;
    _chunk_state = CHUNK_SIZE;
    _cur_chunk_len = 0;
    _has_cur_chunk_len = false;
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
    _recv_boundary_head.clear();
    _recv_body_length = 0;
    _recv_boundary_status = BOUNDARY_RECV_HEAD;
    _has_content_length = false;
    _content_length = 0;
    _chunked = false;
    _chunk_state = CHUNK_SIZE;
    _chunk_buf.clear();
    _cur_chunk_len = 0;
    _has_cur_chunk_len = false;
    _chunked_total = 0;
}

uint64_t http_res_process::body_limit()
{
    uint64_t limit = static_cast<uint64_t>(myframe::max_http_body_bytes());
    if (limit > static_cast<uint64_t>(MAX_CHUNKED_BODY_LEN))
        limit = static_cast<uint64_t>(MAX_CHUNKED_BODY_LEN);
    return limit;
}


size_t http_res_process::process_recv_body(const char *buf, size_t len, int &result)
{
    result = 0;
    size_t ret = 0;
    // Chunked framing wins over the method: even a GET with a chunked body
    // must be consumed frame by frame or the connection loses sync.
    if (_chunked)
    {
        return process_chunked(buf, len, result);
    }
    // Message framing is independent of the method. A bodyless GET/HEAD must
    // consume zero body bytes; the remaining buffer can contain the next request.
    if (_boundary_para._boundary_str.length() == 0)
    {
        if (!_has_content_length)
        {
            // No Content-Length header: finish immediately to avoid hanging.
            // Do not consume current bytes here; they may belong to next request.
            result = 1;
            ret = 0;
        }
        else
        {
            const uint64_t content_length = _content_length;
            if (_recv_body_length >= content_length)
            {
                // Content-Length: 0 or body already complete.
                // Leave current bytes for next request in keep-alive/pipeline.
                result = 1;
                ret = 0;
            }
            else
            {
                uint64_t remain = content_length - _recv_body_length;
                size_t take = (static_cast<uint64_t>(len) < remain) ? len : static_cast<size_t>(remain);
                // The data callback reports whether it has handled this chunk,
                // not whether the HTTP message is complete. A header-only read
                // can call it with take=0 and result=1.
                int data_result = 0;
                ret = _data_process->process_recv_body(buf, take, data_result);
                if (ret > take) ret = take;
                _recv_body_length += ret;
            }
            result = _recv_body_length >= content_length ? 1 : 0;
        }
    }
    else //parse boundary
    {
        ret = get_boundary(buf, len, result);
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
    // A NUL byte can truncate C-string based processing differently in
    // different components (and in upstream proxies): reject it outright.
    if (recv_head.find('\0') != std::string::npos)
    {
        THROW_COMMON_EXCEPT("NUL byte in http header block");
    }

    _has_content_length = false;
    _content_length = 0;
    _chunked = false;

    bool seen_te = false;
    bool seen_host = false;
    size_t header_count = 0;
    size_t line_no = 0;
    size_t start = 0;
    const size_t total = recv_head.size();
    while (start < total)
    {
        size_t pos = recv_head.find(CRLF, start);
        std::string line = (pos == std::string::npos)
                               ? recv_head.substr(start)
                               : recv_head.substr(start, pos - start);
        start = (pos == std::string::npos) ? total : pos + 2;

        if (line.find('\r') != std::string::npos || line.find('\n') != std::string::npos)
        {
            THROW_COMMON_EXCEPT("bare CR or LF in http header block");
        }

        if (line_no++ == 0)
        {
            parse_first_line(line);
            continue;
        }
        if (line.empty())
        {
            break;  // end of the header section
        }
        // RFC 9112 5.2: obsolete line folding is not accepted.
        if (line[0] == ' ' || line[0] == '\t')
        {
            THROW_COMMON_EXCEPT("obs-fold header continuation line rejected");
        }
        if (++header_count > myframe::MAX_HTTP_HEADER_COUNT)
        {
            THROW_COMMON_EXCEPT("too many http header fields (> " << myframe::MAX_HTTP_HEADER_COUNT << ")");
        }
        size_t colon = line.find(':');
        if (colon == std::string::npos)
        {
            THROW_COMMON_EXCEPT("http header line without colon");
        }
        // RFC 9112 5.1: no whitespace is allowed between the field name and
        // the colon. The name must be a non-empty token.
        std::string k = line.substr(0, colon);
        if (k.empty())
        {
            THROW_COMMON_EXCEPT("empty http header field name");
        }
        for (size_t ci = 0; ci < k.size(); ++ci)
        {
            if (!myframe::is_http_token_char(static_cast<unsigned char>(k[ci])))
            {
                THROW_COMMON_EXCEPT("invalid character in http header field name");
            }
        }
        // Strip optional whitespace (OWS) around the value, otherwise
        // "Authorization: Bearer x" keeps a leading space and consumers
        // matching rfind("Bearer ", 0) fail.
        std::string v = line.substr(colon + 1);
        size_t a = v.find_first_not_of(" \t");
        size_t b = v.find_last_not_of(" \t");
        v = (a == std::string::npos) ? std::string() : v.substr(a, b - a + 1);
        for (size_t vi = 0; vi < v.size(); ++vi)
        {
            unsigned char c = static_cast<unsigned char>(v[vi]);
            if ((c < 0x20 && c != '\t') || c == 0x7f)
            {
                THROW_COMMON_EXCEPT("control character in http header value");
            }
        }

        if (strcasecmp(k.c_str(), "Content-Length") == 0)
        {
            // Any repeated Content-Length (even with identical values or a
            // different letter case) is rejected: the strictest framing rule.
            if (_has_content_length)
            {
                THROW_COMMON_EXCEPT("duplicate content-length header");
            }
            uint64_t cl = 0;
            if (!myframe::parse_strict_decimal_u64(v, cl))
            {
                THROW_COMMON_EXCEPT("invalid content-length value");
            }
            _has_content_length = true;
            _content_length = cl;
        }
        else if (strcasecmp(k.c_str(), "Transfer-Encoding") == 0)
        {
            if (seen_te)
            {
                THROW_COMMON_EXCEPT("duplicate transfer-encoding header");
            }
            seen_te = true;
        }
        else if (strcasecmp(k.c_str(), "Host") == 0)
        {
            // RFC 9112 3.2: more than one Host field must be rejected.
            if (seen_host)
            {
                THROW_COMMON_EXCEPT("duplicate host header");
            }
            seen_host = true;
        }
        _req_head_para._headers.insert(make_pair(k, v));
        PDEBUG("%s: %s", k.c_str(), v.c_str());
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

    // Transfer-Encoding (RFC 9112 6/7):
    //   - only a single "chunked" coding is accepted; anything else closes
    //     the connection;
    //   - Transfer-Encoding together with Content-Length is rejected (the
    //     classic request smuggling vector, RFC 9112 6.3).
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
        if (_has_content_length)
        {
            THROW_COMMON_EXCEPT("both transfer-encoding and content-length (smuggling guard)");
        }
        _chunked = true;
        _chunk_state = CHUNK_SIZE;
        _chunk_buf.clear();
        _cur_chunk_len = 0;
        _has_cur_chunk_len = false;
        _chunked_total = 0;
    }

    // Enforce the request body cap before a single body byte is buffered.
    if (_has_content_length &&
        _content_length > static_cast<uint64_t>(myframe::max_http_body_bytes()))
    {
        THROW_COMMON_EXCEPT("http request body too large (content-length " << _content_length
                            << " > " << myframe::max_http_body_bytes() << ")");
    }
}

// Chunked body decoder. Wire format:
//   chunk-size[;ext] CRLF data CRLF ... 0 CRLF [trailer lines...] CRLF
// Returns the number of bytes of buf consumed; bytes after the terminator are
// left to the caller (they belong to the next request under keep-alive).
// Decoded data is handed to the business data process like a plain body;
// result=1 on completion.
size_t http_res_process::process_chunked(const char *buf, size_t len, int &result)
{
    result = 0;
    _chunk_buf.append(buf, len);
    const uint64_t limit = body_limit();
    for (;;)
    {
        if (_chunk_state == CHUNK_SIZE)
        {
            size_t pos = _chunk_buf.find(CRLF);
            if (pos == std::string::npos)
            {
                if (_chunk_buf.length() > MAX_CHUNK_LINE_LEN)
                    THROW_COMMON_EXCEPT("chunk size line too long");
                return len;   // need more data (everything buffered)
            }
            if (pos > MAX_CHUNK_LINE_LEN)
                THROW_COMMON_EXCEPT("chunk size line too long");
            std::string line = _chunk_buf.substr(0, pos);
            size_t semi = line.find(';');
            if (semi != std::string::npos) line.erase(semi);   // chunk extensions are ignored
            // Only bad whitespace (BWS) before ';' is tolerated.
            size_t last = line.find_last_not_of(" \t");
            line.erase(last == std::string::npos ? 0 : last + 1);
            uint64_t sz64 = 0;
            if (!myframe::parse_strict_hex_u64(line, sz64))
                THROW_COMMON_EXCEPT("bad chunk size line");
            // Overflow-safe bound check before accumulating.
            if (sz64 > limit || _chunked_total > limit - sz64)
                THROW_COMMON_EXCEPT("chunked body too large (limit " << limit << ")");
            _chunk_buf.erase(0, pos + 2);
            if (sz64 == 0)
            {
                _chunk_state = CHUNK_TRAILER;
                continue;
            }
            _cur_chunk_len = static_cast<size_t>(sz64);
            _has_cur_chunk_len = true;
            _chunk_state = CHUNK_DATA;
        }
        else if (_chunk_state == CHUNK_DATA)
        {
            if (!_has_cur_chunk_len)
                THROW_COMMON_EXCEPT("chunk decoder state error");
            if (_chunk_buf.length() < _cur_chunk_len + 2)
                return len;   // wait for the whole chunk (total bounded by body_limit())
            if (_chunk_buf[_cur_chunk_len] != '\r' || _chunk_buf[_cur_chunk_len + 1] != '\n')
                THROW_COMMON_EXCEPT("chunk data not CRLF terminated");
            int ignore = 0;
            _data_process->process_recv_body(_chunk_buf.c_str(), _cur_chunk_len, ignore);
            _chunked_total += static_cast<uint64_t>(_cur_chunk_len);
            _chunk_buf.erase(0, _cur_chunk_len + 2);
            _cur_chunk_len = 0;
            _has_cur_chunk_len = false;
            _chunk_state = CHUNK_SIZE;
        }
        else // CHUNK_TRAILER: consume lines until the empty line (trailers ignored)
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
                // Bytes after the terminator belong to the next request: leave
                // them in the caller's buffer (only count what was consumed).
                size_t leftover = _chunk_buf.length();
                _chunk_buf.clear();
                return (leftover <= len) ? (len - leftover) : 0;   // stream order guarantees leftover <= len
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
    uint64_t content_length = _has_content_length ? _content_length : 0;

    if (!content_length)
    {
        THROW_COMMON_EXCEPT("get boundary but content_len not found");
    }
    size_t ret = len;
    size_t p_len = 0;
    result = 0;
    _recv_body_length += len;
    // First locate the boundary part header
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
            else // nothing to deliver yet
            {
            }
        }
        else // boundary part header still incomplete
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
        else // plain body bytes
        {
        }

        p_len = _data_process->process_recv_body(buf, tmp_len, result);
        p_len = tmp_len - p_len;

        if (_recv_body_length == content_length)
            result = 1; // body complete
    }
    else //recv tail
    {	
        // trailing boundary: nothing to deliver, just consume
        if (_recv_body_length == content_length)
            result = 1;				
    }
    _recv_body_length = _recv_body_length - p_len;
    return ret - p_len;
}
