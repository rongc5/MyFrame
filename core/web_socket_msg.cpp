#include "web_socket_msg.h"

#include "common_exception.h"
#include "common_util.h"


/*
   websocket 协议字段
   0					1					2					3
   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-------+-+-------------+-------------------------------+
   |M|R|R|R| opcode|R| Payload len |	 Extended payload length	|
   |O|S|S|S|  (4)	|S| 	(7) 	|			  (16/63)			|
   |R|V|V|V|		|V| 			|	(if payload len==126/127)	|
   |E|1|2|3|		|4| 			|								|
   +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
   |	  Extended payload length continued, if payload len == 127	|
   + - - - - - - - - - - - - - - - +-------------------------------+
   |								|		  Extension data		|
   +-------------------------------+ - - - - - - - - - - - - - - - +
   :																:
   +---------------------------+
   :						Application data						:
   +---------------------------+


   0                   1                   2                   3
   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-------+-+-------------+-------------------------------+
   |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
   |I|S|S|S|  (4)  |A|     (7)     |             (16/63)           |
   |N|V|V|V|       |S|             |   (if payload len==126/127)   |
   | |1|2|3|       |K|             |                               |
   +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
   |     Extended payload length continued, if payload len == 127  |
   + - - - - - - - - - - - - - - - +-------------------------------+
   |                               |Masking-key, if MASK set to 1  |
   +-------------------------------+-------------------------------+
   | Masking-key (continued)       |          Payload Data         |
   +-------------------------------- - - - - - - - - - - - - - - - +
   :                     Payload Data continued ...                :
   + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
   |                     Payload Data continued ...                |
   +---------------------------+

*/	
web_socket_frame_header::web_socket_frame_header()
{
    clear();
}

void web_socket_frame_header::clear()
{
    _s_header.clear();
    _wb_body_status = WB_FRAME_HEAD_STAUS;
    _payload_len = 0;
    _mask_flag = -1;
    _e_p_len = 0;
    _process_body_len = 0;
    _mask_offset = 0;
    _mask_key.clear();
    _op_code = 1;
    _more_flag=0;
}

// Incrementally collect the frame header from buf. Only the bytes the header
// still needs are copied (2-byte base header, then the 0/2/8-byte extended
// length, then the optional 4-byte masking key: at most 14 bytes in total),
// so the cost per frame is O(1) regardless of how much data follows in buf.
// On return buf/len describe the bytes after the header (len == 0 when the
// header is still incomplete and every input byte was absorbed).
void web_socket_frame_header::process(char* &buf,   uint32_t &len)
{
    for (;;)
    {
        const size_t need = (_mask_flag == -1)
                                ? 2u
                                : (2u + _e_p_len + (_mask_flag == 1 ? 4u : 0u));
        if (_s_header.length() < need)
        {
            if (len == 0)
            {
                break;  // header incomplete: wait for more data
            }
            size_t want = need - _s_header.length();
            size_t take = (static_cast<size_t>(len) < want) ? static_cast<size_t>(len) : want;
            _s_header.append(buf, take);
            buf += take;
            len -= static_cast<uint32_t>(take);
            if (_s_header.length() < need)
            {
                break;
            }
        }

        if (_mask_flag == -1)
        {
            // Base header complete: decode FIN/RSV/opcode/MASK/payload len.
            _op_code = _s_header[0]  & 0xF;
            _more_flag = (_s_header[0] >> 7) & 0x01;
            uint8_t rsv = (_s_header[0] >> 4) & 0x07;
            if (rsv != 0)
            {
                THROW_COMMON_EXCEPT("websocket RSV bits are not supported");
            }
            // Reject opcodes reserved by RFC 6455 5.2 (must fail the connection).
            if (_op_code != 0x0 && _op_code != 0x1 && _op_code != 0x2 &&
                _op_code != 0x8 && _op_code != 0x9 && _op_code != 0xA)
            {
                THROW_COMMON_EXCEPT("websocket reserved opcode " << (int)_op_code);
            }
            // RFC 6455 5.4 fragmentation state machine:
            // - CONTINUATION (0x0) requires an active fragmented data message.
            // - A new data frame (TEXT/BINARY) cannot start while a fragmented
            //   data message is still open. Control frames may interleave freely.
            if (_op_code == 0x0 && _message_op_code == 0)
            {
                THROW_COMMON_EXCEPT("unexpected websocket continuation frame");
            }
            if ((_op_code == 0x1 || _op_code == 0x2) && _message_op_code != 0)
            {
                THROW_COMMON_EXCEPT("websocket data frame received mid-fragmentation");
            }
            _mask_flag = (_s_header[1] >> 7) & 0x01;
            uint8_t tmp_len = (_s_header[1] & 0x7F);
            if (_op_code >= 0x08 && (_more_flag == 0 || tmp_len > 125))
            {
                THROW_COMMON_EXCEPT("invalid websocket control frame");
            }
            // RFC 6455 5.5.1: a Close frame body is either empty or contains
            // at least a 2-byte status code. A 1-byte payload is illegal and
            // MUST fail the connection.
            if (_op_code == 0x08 && tmp_len == 1)
            {
                THROW_COMMON_EXCEPT("invalid close frame: payload length 1");
            }
            if (tmp_len < 126)
            {
                _e_p_len = 0;
            }
            else if (tmp_len == 126)
            {
                _e_p_len = 2;
            }
            else //127
            {
                _e_p_len = 8;
            }
            continue;  // re-evaluate how many header bytes are needed
        }

        // Full header available.
        if (_mask_flag == 1)
        {
            _mask_key.assign(_s_header, 2 + _e_p_len, 4);
        }
        else
        {
            _mask_key.clear();
        }
        get_payload_length();
        _wb_body_status = WB_FRAME_BODY_STAUS;
        break;
    }
}

std::string web_socket_frame_header::gen_ping_header(const int8_t op_code/*0x09 or 0x0A*/, const std::string &ping_data)//ping or pong
{
    return gen_control_frame(op_code, ping_data, std::string());
}

std::string web_socket_frame_header::gen_control_frame(const int8_t op_code, const std::string &payload, const std::string &mask_key)
{
    if (payload.length() > 125)
    {
        THROW_COMMON_EXCEPT("websocket control frame payload too long " << payload.length());
    }

    const bool masked = !mask_key.empty();
    if (masked && mask_key.length() != 4)
    {
        THROW_COMMON_EXCEPT("websocket mask key length is not 4");
    }

    std::string frame;
    char aa = 0;
    aa = aa | (0x01 << 7);
    aa = aa | (op_code & 0x0f);
    frame.append(1, aa);

    char bb = static_cast<char>(payload.length());
    if (masked)
    {
        bb = bb | static_cast<char>(0x80);
    }
    frame.append(1, bb);

    if (masked)
    {
        frame.append(mask_key.c_str(), 4);
        for (size_t i = 0; i < payload.length(); ++i)
        {
            frame.append(1, payload[i] ^ mask_key[i % 4]);
        }
    }
    else
    {
        frame.append(payload);
    }
    return frame;
}

std::string web_socket_frame_header::gen_frame_header(const uint64_t data_len, const std::string &mask_key, const int8_t content_type)
{	
    if (!mask_key.empty())
    {
        _mask_flag = 1;
        _mask_key = mask_key;
    }
    else
    {
        _mask_flag = 0;
    }

    if (content_type >= 0x08 && data_len > 125)
    {
        THROW_COMMON_EXCEPT("websocket control frame payload too long " << data_len);
    }

    _op_code = content_type;
    _payload_len = data_len;
    std::string frame_header;

    char aa = 0;
    aa = aa | (0x01 << 7);
    aa = aa | _op_code;
    frame_header.append(1, aa);
    if (data_len < 126)
    {
        char tmp = data_len;
        tmp = tmp | (_mask_flag << 7);
        frame_header.append(1, tmp);
    }
    else if (data_len <= MAX_INT16_NUM)//16 bit int
    {
        char tmp = 126;
        tmp = tmp | (_mask_flag << 7);
        frame_header.append(1, tmp);
        uint16_t len = data_len;
        len = htons(len);
        frame_header.append((char*)&len, 2);
    }
    else //64 bit int
    {
        char tmp = 127;
        tmp = tmp | (_mask_flag << 7);
        frame_header.append(1, tmp);
        uint64_t tmp_len = htonl64(data_len);
        frame_header.append((char*)&tmp_len, 8);
    }

    if (_mask_flag == 1)
    {
        frame_header.append(mask_key.c_str(), 4);
    }
    _wb_body_status = WB_FRAME_BODY_STAUS;
    return frame_header;
}

uint32_t web_socket_frame_header::update(const uint32_t len)
{	
    uint32_t ret = 0;
    if (_process_body_len + len < _payload_len)
    {
        _process_body_len += len;
        ret = 0;
    }
    else
    {	
        ret =_process_body_len + len - _payload_len;              
        _process_body_len = _payload_len;
    }
    return ret;
}

bool web_socket_frame_header::if_finish()
{
    // A frame is complete once all declared body bytes have been consumed.
    // Empty payloads (legitimate for close/ping/pong and empty data frames,
    // including the final empty CONTINUATION of a fragmented message) are
    // complete immediately because process_body_len and payload_len are both 0.
    bool ret = (_process_body_len == _payload_len);
    if (ret)
    {
        if (_op_code < 0x08)
        {
            // Track the message-level opcode across data fragments so that
            // the final CONTINUATION frame can report TEXT/BINARY upstream.
            if (_op_code != 0x0)
            {
                _message_op_code = _op_code;
            }
            _completed_op_code = (_message_op_code != 0) ? _message_op_code : _op_code;
            if (_more_flag != 0)
            {
                _message_op_code = 0; // message complete; reset for next
            }
        }
        else
        {
            _completed_op_code = _op_code;
        }
        _completed_fin = _more_flag;
        clear();
    }
    return ret && (_completed_fin != 0);
}

std::string web_socket_frame_header::mask_code(const char *p_buf, const size_t len)
{		
    std::string ret;
    ret.resize(len);
    for (uint32_t ii=0; ii < len; ++ii)
    {
        ret[ii] = p_buf[ii] ^ _mask_key[_mask_offset];
        _mask_offset = (_mask_offset + 1)%4;
    }
    return ret;
}

void web_socket_frame_header::get_payload_length()
{
    if (_e_p_len == 0)	
    {
        _payload_len = (_s_header[1] & 0x7F);			
    }
    else if (_e_p_len == 2)
    {
        uint16_t tmp = 0;
        memcpy(&tmp , _s_header.c_str() + 2, sizeof(tmp));
        _payload_len = ntohs(tmp);
    }
    else 
    {
        memcpy(&_payload_len , _s_header.c_str() + 2, 8);
        _payload_len = ntohl64(_payload_len);
    }
    if (_payload_len >= MAX_PAYLOAD_LEN)
        THROW_COMMON_EXCEPT("PAYLOAD_LEN too long " << _payload_len);
}


ws_msg_type::ws_msg_type()
{   
    init();
}   

void ws_msg_type::init()
{
    _p_msg = NULL;
    _con_type = 0x01;
}

ws_req_head_para::ws_req_head_para()
{
    _version = 13;
    _s_websocket_key = WEB_SOCKET_NONCE_KEY;
}
