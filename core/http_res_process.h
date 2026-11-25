#ifndef __HTTP_RES_PROCESS_H__
#define __HTTP_RES_PROCESS_H__

#include "base_def.h"
#include "base_net_obj.h"
#include "base_singleton.h"
#include "base_data_process.h"
#include "common_exception.h"
#include "common_def.h"
#include "http_base_process.h"
#include "protocol_limits.h"

class http_res_process:public http_base_process
{
    public:
        http_res_process(std::shared_ptr<base_net_obj> p);

        virtual ~http_res_process();

		virtual void reset();     

		// Buffered (pipelined) input can only be parsed while waiting for a
		// request; during the response phase it stays queued.
		virtual bool can_process_buffered_input() const { return _http_status <= RECV_BODY; }

    protected:
		virtual size_t process_recv_body(const char *buf, size_t len, int &result);
        
        virtual void parse_first_line(const std::string & line);

        virtual void parse_header(std::string & recv_head);

        virtual void recv_finish();

        virtual void send_finish();

		size_t get_boundary(const char *buf, size_t len, int &result);

		// Chunked request body decoder (RFC 9112 7.1: an HTTP/1.1 server must
		// support it; proxies such as cloudflared forward bodies as chunked).
		// Safety rules: Transfer-Encoding together with Content-Length is
		// rejected (smuggling guard); only a single "chunked" coding is
		// accepted; decoded size and line lengths are capped; trailers are
		// consumed and ignored. The decoded body looks like a plain body to
		// the business layer.
		size_t process_chunked(const char *buf, size_t len, int &result);

        static uint64_t body_limit();

    protected:
		enum BOUNDARY_STATUS
		{
			BOUNDARY_RECV_HEAD = 0,
			BOUNDARY_RECV_BODY = 1,
			BOUNDARY_RECV_TAIL = 2
		};
        std::string _recv_boundary_head;
		boundary_para _boundary_para;
		BOUNDARY_STATUS _recv_boundary_status;
		uint64_t _recv_body_length;

		// Framing state decided by parse_header().
		bool        _has_content_length;
		uint64_t    _content_length;

		// ---- chunked decoder state ----
		enum CHUNK_STATE
		{
			CHUNK_SIZE = 0,     // waiting for a chunk-size line
			CHUNK_DATA = 1,     // waiting for the current chunk data (+CRLF)
			CHUNK_TRAILER = 2   // after the last chunk: consume trailer lines up to the empty line
		};
		bool        _chunked;            // request body uses chunked coding
		CHUNK_STATE _chunk_state;
		std::string _chunk_buf;          // undecoded input
		size_t      _cur_chunk_len;      // current chunk data length (valid when _has_cur_chunk_len)
		bool        _has_cur_chunk_len;
		uint64_t    _chunked_total;      // decoded bytes so far (bounded)
};

// Legacy default. Runtime policy for both body encodings is max_http_body_bytes().
#define MAX_CHUNKED_BODY_LEN (16u * 1024u * 1024u)
// Maximum chunk-size or trailer line length.
#define MAX_CHUNK_LINE_LEN   8192u


#endif

