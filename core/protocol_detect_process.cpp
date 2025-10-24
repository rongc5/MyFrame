#include "protocol_detect_process.h"
#include "protocol_probes.h"
#include "common_exception.h"
#include "base_connect.h"
#include "base_timer.h"
#include "common_util.h"
#include <sys/socket.h>
#include <cstring>

protocol_detect_process::protocol_detect_process(std::shared_ptr<base_net_obj> base_obj, myframe::IApplicationHandler* app_handler, bool over_tls)
    : base_data_process(base_obj)
    , _protocol_detected(false)
    , _over_tls(over_tls)
    , _app_handler(app_handler)
    , _total_bytes(0)
    , _start_ms(0)
    , _timer_id(0)
    , _detect_timeout_ms(kDetectTimeoutMs)
{ }

protocol_detect_process::~protocol_detect_process() {}

size_t protocol_detect_process::process_recv_buf(const char* buf, size_t buf_len)
{
    if (_protocol_detected) {
        // 已切换后直接转发
        base_connect<base_data_process>* holder = dynamic_cast< base_connect<base_data_process>* >(get_base_net().get());
        if (holder && holder->process()) {
            return holder->process()->process_recv_buf(buf, buf_len);
        }
        return buf_len;
    }

    if (_start_ms == 0) {
        _start_ms = GetMilliSecond();
        std::shared_ptr<timer_msg> t(new timer_msg);
        t->_obj_id = get_base_net()->get_id()._id; t->_timer_type = NONE_DATA_TIMER_TYPE; t->_time_length = _detect_timeout_ms;
        add_timer(t); _timer_id = t->_timer_id;
    }

    // Try probes
    for (size_t i = 0; i < _probes.size(); ++i) {
        IProtocolProbe* probe = _probes[i].get();
        if (probe->match(buf, buf_len)) {
            _protocol_detected = true;

            base_connect<base_data_process>* holder = dynamic_cast< base_connect<base_data_process>* >(get_base_net().get());
            if (holder) {
                std::unique_ptr<base_data_process> next = probe->create(get_base_net());
                // base_connect 将接管生命周期（包装为 unique_ptr）
                holder->set_process(next.release());

                // 让新流程预处理这批数据，但按探测缓冲长度告知上层擦除
                if (holder->process()) {
                    (void)holder->process()->process_recv_buf(buf, buf_len);
                    return buf_len;
                }
            }
            return 0;
        }
    }
    // Update total sniffed bytes and enforce limit
    if (buf_len > 0) {
        // Guard against overflow though impractical here
        if (_total_bytes > SIZE_MAX - buf_len) {
            THROW_COMMON_EXCEPT("protocol detect overflow");
            return 0;
        }
        _total_bytes += buf_len;
        if (_total_bytes > kMaxDetectBytes) {
            THROW_COMMON_EXCEPT("Too much data without protocol detection");
            return 0;
        }
    }
    return 0; // 不消费，等待更多数据
}

void protocol_detect_process::handle_timeout(std::shared_ptr<timer_msg>& t)
{
    if (t->_timer_id != _timer_id) return;
    if (!_protocol_detected) {
        THROW_COMMON_EXCEPT("protocol detect timeout");
    }
}
