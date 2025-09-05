#include "multi_protocol_factory.h"
#include "common_def.h"
#include "common_obj_container.h"
#include "base_net_thread.h"
#include "base_net_obj.h"
#include "base_connect.h"
#include "protocol_detect_process.h"
#include "protocol_probes.h"
#include <atomic>

class MultiProtocolFactoryImplAccessor {
public:
    explicit MultiProtocolFactoryImplAccessor(MultiProtocolFactory* f) : _f(f) {}
    void set_container(common_obj_container* c) { _f->_container = c; }
    common_obj_container* container() const { return _f->_container; }
private:
    MultiProtocolFactory* _f;
};

void MultiProtocolFactory::net_thread_init(base_net_thread* th)
{
    if (!th) return;
    MultiProtocolFactoryImplAccessor acc(this);
    acc.set_container(th->get_net_container());
}

void MultiProtocolFactory::handle_thread_msg(std::shared_ptr<normal_msg>& msg)
{
    if (!msg || msg->_msg_op != NORMAL_MSG_CONNECT) return;
    fprintf(stderr, "[factory] NORMAL_MSG_CONNECT received\n");
    MultiProtocolFactoryImplAccessor acc(this);
    common_obj_container* container = acc.container();
    if (!container) { fprintf(stderr, "[factory] container is null\n"); return; }

    std::shared_ptr<content_msg> cm = std::static_pointer_cast<content_msg>(msg);
    int fd = cm->fd;
    fprintf(stderr, "[factory] creating connection for fd=%d\n", fd);

    std::shared_ptr< base_connect<base_data_process> > conn(new base_connect<base_data_process>(fd));
    std::unique_ptr<protocol_detect_process> detector(new protocol_detect_process(conn, _app_handler));

    if (_mode == Mode::Auto) {
        detector->add_probe(std::unique_ptr<IProtocolProbe>(new TlsProbe(_app_handler)));
        detector->add_probe(std::unique_ptr<IProtocolProbe>(new WsProbe(_app_handler)));
        detector->add_probe(std::unique_ptr<IProtocolProbe>(new HttpProbe(_app_handler)));
        detector->add_probe(std::unique_ptr<IProtocolProbe>(new CustomProbe(_app_handler)));
    } else if (_mode == Mode::TlsOnly) {
        detector->add_probe(std::unique_ptr<IProtocolProbe>(new TlsProbe(_app_handler)));
    } else { // PlainOnly
        detector->add_probe(std::unique_ptr<IProtocolProbe>(new WsProbe(_app_handler)));
        detector->add_probe(std::unique_ptr<IProtocolProbe>(new HttpProbe(_app_handler)));
        detector->add_probe(std::unique_ptr<IProtocolProbe>(new CustomProbe(_app_handler)));
    }

    conn->set_process(detector.release());
    conn->set_net_container(container);
    std::shared_ptr<base_net_obj> net_obj = conn;
    container->push_real_net(net_obj);
    fprintf(stderr, "[factory] connection enqueued to container thread=%u\n", container->get_thread_index());
}

void MultiProtocolFactory::register_worker(uint32_t thread_index)
{
    _worker_indices.push_back(thread_index);
}

void MultiProtocolFactory::on_accept(base_net_thread* listen_th, int fd)
{
    if (!listen_th) return;
    uint32_t target_index = listen_th->get_thread_index();
    if (!_worker_indices.empty()) {
        unsigned long idx = _rr_hint++;
        target_index = _worker_indices[idx % _worker_indices.size()];
    }
    std::shared_ptr<content_msg> cm(new content_msg());
    cm->fd = fd;
    std::shared_ptr<normal_msg> ng = std::static_pointer_cast<normal_msg>(cm);
    ObjId id; id._id = OBJ_ID_THREAD; id._thread_index = target_index;
    base_net_thread::put_obj_msg(id, ng);
}
