#ifndef __HTTP_RES_PROCESS_H__
#define __HTTP_RES_PROCESS_H__

#include "base_def.h"
#include "base_net_obj.h"
#include "base_singleton.h"
#include "base_data_process.h"
#include "common_exception.h"
#include "common_def.h"
#include "http_base_process.h"

class http_res_process:public http_base_process
{
    public:
        http_res_process(std::shared_ptr<base_net_obj> p);

        virtual ~http_res_process();

		virtual void reset();     

    protected:
		virtual size_t process_recv_body(const char *buf, size_t len, int &result);
        
        virtual void parse_first_line(const std::string & line);

        virtual void parse_header(std::string & recv_head);

        virtual void recv_finish();

        virtual void send_finish();

		size_t get_boundary(const char *buf, size_t len, int &result);

		// chunked 请求体解码（RFC 9112 §7.1：HTTP/1.1 服务器必须支持;cloudflared 等代理会用 chunked 转发 body）。
		// 安全约束：TE 与 Content-Length 并存 → 拒绝(请求走私防护);仅支持单一 "chunked";
		//           解码总量/行长有上限;trailer 消费但忽略。解码后对业务层等价于普通完整 body。
		size_t process_chunked(const char *buf, size_t len, int &result);

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
		uint32_t _recv_body_length;

		// ---- chunked 解码状态 ----
		enum CHUNK_STATE
		{
			CHUNK_SIZE = 0,     // 等 chunk-size 行
			CHUNK_DATA = 1,     // 等当前 chunk 数据(+CRLF)
			CHUNK_TRAILER = 2   // 0 块之后:消费 trailer 行直到空行
		};
		bool        _chunked;         // 本请求体为 chunked（parse_header 判定）
		CHUNK_STATE _chunk_state;
		std::string _chunk_buf;       // 尚未解码完的输入
		long        _cur_chunk_len;   // 当前 chunk 数据长；-1 = 等 size 行
		uint64_t    _chunked_total;   // 已解码总量（上限防护）
};

// chunked 防护上限：解码后 body 总量 / 单行(size 行与 trailer 行)长度
#define MAX_CHUNKED_BODY_LEN (8u * 1024u * 1024u)
#define MAX_CHUNK_LINE_LEN   8192u


#endif

