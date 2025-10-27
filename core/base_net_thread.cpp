#include "base_net_thread.h"
#include "channel_data_process.h"
#include "common_obj_container.h"
#include "base_net_obj.h"
#include "base_connect.h"
#include "common_util.h"
#include "factory_base.h"
#include <algorithm>

base_net_thread::base_net_thread(int channel_num):_channel_num(channel_num), _base_container(NULL), _factory_for_thread(nullptr){
    net_thread_init();
}

base_net_thread::base_net_thread(IFactory* factory, int channel_num)
    : _channel_num(channel_num), _base_container(NULL), _factory_for_thread(factory) {
    net_thread_init();
}

base_net_thread::~base_net_thread(){
    if (_base_container){
        delete _base_container;
    }
}

void * base_net_thread::run()
{
    while (get_run_flag()) {
        run_process();
        _base_container->obj_process();
        for (size_t i = 0; i < _plugins.size(); ++i) {
            if (_plugins[i]) _plugins[i]->on_tick();
        }
    }

    return NULL;
}

void base_net_thread::run_process()
{

}

void base_net_thread::net_thread_init()
{
    _base_container = new common_obj_container(get_thread_index());
    for (size_t i = 0; i < _plugins.size(); ++i) {
        if (_plugins[i]) _plugins[i]->on_init(this);
    }
    if (_factory_for_thread) {
        _factory_for_thread->net_thread_init(this);
    }

    for (int i = 0; i < _channel_num; i++) {

        int fd[2];
        int ret = socketpair(AF_UNIX,SOCK_STREAM,0,fd);
        if (ret < 0) {
            return ;
        }   

        std::shared_ptr<base_connect<channel_data_process> >  channel_connect(new base_connect<channel_data_process>(fd[0]));
        channel_data_process * data_process = new channel_data_process(channel_connect, fd[1]);
        channel_connect->set_process(data_process);
        channel_connect->set_net_container(_base_container);

        _channel_msg_vec.push_back(channel_connect);
    }


    _base_net_thread_map[get_thread_index()] = this;
}


void base_net_thread::put_msg(uint32_t obj_id, std::shared_ptr<normal_msg> & p_msg)
{
    int index = (unsigned long) (&p_msg) % _channel_msg_vec.size();
    std::shared_ptr<base_connect<channel_data_process> >  channel_connect = _channel_msg_vec[index];
    channel_data_process * data_process = channel_connect->process();
    if (data_process)
    {
        data_process->put_msg(obj_id, p_msg);
        //channel_connect->set_real_net(true); 风险
    }
}

void base_net_thread::handle_msg(std::shared_ptr<normal_msg> & p_msg)
{
    if (!p_msg) {
        return;
    }

    if (handle_thread_msg(p_msg)) {
        return;
    }

    if (_factory_for_thread) {
        _factory_for_thread->handle_thread_msg(p_msg);
    }
}

bool base_net_thread::handle_thread_msg(std::shared_ptr<normal_msg> &)
{
    return false;
}

base_net_thread * base_net_thread::get_base_net_thread_obj(uint32_t thread_index)
{
    std::unordered_map<uint32_t, base_net_thread *>::const_iterator it = _base_net_thread_map.find(thread_index);
    if (it != _base_net_thread_map.end()){
        return it->second;
    }

    return NULL;
}

void base_net_thread::add_timer(std::shared_ptr<timer_msg> & t_msg)
{
    _base_container->add_timer(t_msg);
}

void base_net_thread::put_obj_msg(ObjId & id, std::shared_ptr<normal_msg> & p_msg)
{
    base_net_thread * net_thread = get_base_net_thread_obj(id._thread_index);
    if (!net_thread) {
        return;
    }

    net_thread->put_msg(id._id, p_msg);
}

void base_net_thread::handle_timeout(std::shared_ptr<timer_msg> & t_msg)
{
    return ;
}

common_obj_container * base_net_thread::get_net_container()
{
    return _base_container;
}

std::unordered_map<uint32_t, base_net_thread *> base_net_thread::_base_net_thread_map;

void base_net_thread::add_worker_thread(uint32_t thread_index)
{
    // 避免重复
    if (std::find(_worker_pool.begin(), _worker_pool.end(), thread_index) == _worker_pool.end()) {
        _worker_pool.push_back(thread_index);
    }
}

uint32_t base_net_thread::next_worker_rr()
{
    if (_worker_pool.empty()) return get_thread_index();
    unsigned long idx = _rr_hint++;
    return _worker_pool[idx % _worker_pool.size()];
}

 
