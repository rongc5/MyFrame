// Framework-internal message that runs a closure against a connection on the
// connection's owner (network) thread.
//
// This is the transport used to keep all socket I/O, _epoll_event mutations
// and protocol send-state changes on the owner thread: a foreign thread never
// touches the socket, it posts an obj_task_msg to the connection's ObjId via
// base_net_thread::put_obj_msg(). common_obj_container::handle_msg() resolves
// the ObjId, verifies the connection generation (so a recycled obj_id can
// never receive another connection's payload) and runs the closure. If the
// connection has already been closed the message is silently dropped.
#ifndef __MYFRAME_OBJ_TASK_MSG_H__
#define __MYFRAME_OBJ_TASK_MSG_H__

#include "common_def.h"
#include <functional>

class base_net_obj;

// Reserved framework op code for obj_task_msg (chosen far away from small
// application-defined op codes).
#define NORMAL_MSG_OBJ_TASK 0x4f424a54  /* 'OBJT' */

class obj_task_msg : public normal_msg
{
    public:
        typedef std::function<void(base_net_obj &)> task_fn;

        obj_task_msg(uint32_t generation, task_fn fn)
            : normal_msg(NORMAL_MSG_OBJ_TASK), _generation(generation), _fn(std::move(fn)) {}

        virtual ~obj_task_msg() {}

        // 0 disables the generation check.
        uint32_t _generation;
        task_fn _fn;
};

// Post fn to the owner thread of connection `id`. Returns false when the
// owner thread cannot be resolved (the closure is then destroyed unrun).
bool post_obj_task(const ObjId & id, uint32_t generation, obj_task_msg::task_fn fn);

#endif
