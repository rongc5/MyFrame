#include "protocol_detect_process.h"
#include "protocol_probes.h"
#include "common_exception.h"
#include "base_connect.h"
#include "base_timer.h"
#include "common_util.h"
#include "base_net_obj.h"
#include <typeinfo>
#include <string>
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
        // Already switched: forward directly
        base_connect<base_data_process>* holder = dynamic_cast< base_connect<base_data_process>* >(get_base_net().get());
        if (holder && holder->process()) {
            return holder->process()->process_recv_buf(buf, buf_len);
        }
        return buf_len;
    }

    if (_start_ms == 0) {
        _start_ms = GetMilliSecond();
        std::shared_ptr<timer_msg> t(new timer_msg);
        t->_obj_id = get_base_net()->get_id()._id; t->_timer_type = PROTOCOL_DETECT_TIMER_TYPE; t->_time_length = _detect_timeout_ms;
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
                // base_connect takes ownership (wrapped in a unique_ptr)
                if (!next) {
                    PDEBUG("[detect] probe create returned null");
                    THROW_COMMON_EXCEPT("protocol detect create failed");
                }

#ifdef ENABLE_SSL
                bool lock_protocol = (dynamic_cast<tls_entry_process*>(next.get()) == nullptr);
#else
                bool lock_protocol = true;
#endif
                if (auto net = get_base_net()) {
                    std::string tag = typeid(*next).name();
                    net->set_protocol_tag(tag, lock_protocol);
                }

                // set_process() deletes this detector: only locals below.
                holder->set_process(next.release());

                // Feed the buffered bytes to the new process and report exactly
                // what it consumed, so an incomplete header or a pipelined
                // request stays in base_connect::_recv_buf instead of being
                // erased.
                if (holder->process()) {
                    size_t consumed = holder->process()->process_recv_buf(buf, buf_len);
                    return consumed > buf_len ? buf_len : consumed;
                }
            }
            return 0;
        }
    }
    // Nothing is consumed while undetected, so buf always holds the whole
    // sniffed prefix: its length is the total number of sniffed bytes.
    _total_bytes = buf_len;
    if (_total_bytes > kMaxDetectBytes) {
        THROW_COMMON_EXCEPT("Too much data without protocol detection");
    }
    return 0; // consume nothing; wait for more data
}

void protocol_detect_process::handle_timeout(std::shared_ptr<timer_msg>& t)
{
    if (t->_timer_id != _timer_id) return;
    if (!_protocol_detected) {
        THROW_COMMON_EXCEPT("protocol detect timeout");
    }
}
