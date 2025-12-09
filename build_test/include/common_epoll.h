#ifndef __COMMON_EPOLL_H_
#define __COMMON_EPOLL_H_

#include "common_def.h"
#include "common_exception.h"
#include "common_util.h"

class base_net_obj;
class common_epoll
{
    public:
        common_epoll()
        {
            _epoll_fd = -1;
            _epoll_events = NULL;
            _epoll_size = 0;
            _epoll_wait_time = DEFAULT_EPOLL_WAITE;
        }

        ~common_epoll()
        {
            cleanup();
        }

        void init(const uint32_t epoll_size = DAFAULT_EPOLL_SIZE, int epoll_wait_time = DEFAULT_EPOLL_WAITE)
        {
            cleanup();
            // Allow override by environment variables
            const char* env_size = ::getenv("MYFRAME_EPOLL_SIZE");
            const char* env_wait = ::getenv("MYFRAME_EPOLL_WAIT_MS");
            const char* preset   = ::getenv("MYFRAME_PERF_PRESET");
            _epoll_size = (epoll_size == 0)?DAFAULT_EPOLL_SIZE:epoll_size;
            if (env_size) {
                long v = atol(env_size);
                if (v > 0) _epoll_size = (uint32_t)v;
            }
            // Clamp epoll size to sane range
            if (_epoll_size < 256) _epoll_size = 256;
            if (_epoll_size > 65536) _epoll_size = 65536;
            _epoll_wait_time = epoll_wait_time;
            if (env_wait) {
                int v = atoi(env_wait); if (v >= 0) _epoll_wait_time = v;
            } else if (preset && (strcmp(preset, "0") != 0 && strcasecmp(preset, "false") != 0)) {
                // Recommended low-latency default when preset enabled
                _epoll_wait_time = 1;
            }
            if (_epoll_wait_time < 0) _epoll_wait_time = 0;
            if (_epoll_wait_time > 1000) _epoll_wait_time = 1000;

            PDEBUG("[epoll] size=%u wait_ms=%d (preset=%s)", _epoll_size, _epoll_wait_time, preset?preset:"off");

            int fd = -1;
#ifdef EPOLL_CLOEXEC
            fd = epoll_create1(EPOLL_CLOEXEC);
            if (fd == -1 && errno != EINVAL && errno != ENOSYS)
            {
                std::string err = strError(errno);
                THROW_COMMON_EXCEPT("epoll_create1 fail " << err.c_str());
            }
            if (fd == -1)
#endif
            {
                fd = epoll_create(_epoll_size);
            }
            if (fd == -1)
            {       
                std::string err = strError(errno);
                THROW_COMMON_EXCEPT("epoll_create fail " << err.c_str());
            }
            _epoll_fd = fd;
            _epoll_events = new epoll_event[_epoll_size];
            memset(_epoll_events, 0, sizeof(epoll_event) * _epoll_size);
        }

        void add_to_epoll(base_net_obj * p_obj);

        void del_from_epoll(base_net_obj * p_obj);

        void mod_from_epoll(base_net_obj * p_obj);

        int epoll_wait(std::map<ObjId, std::shared_ptr<base_net_obj> > &expect_list, std::map<ObjId, std::shared_ptr<base_net_obj> > &remove_list, uint32_t num);

    private:
        int _epoll_fd;
        struct epoll_event *_epoll_events;
        uint32_t _epoll_size;
        int _epoll_wait_time;

        void cleanup()
        {
            if (_epoll_fd >= 0)
            {
                ::close(_epoll_fd);
                _epoll_fd = -1;
            }
            if (_epoll_events != NULL)
            {
                delete [] _epoll_events;
                _epoll_events = NULL;
            }
            _epoll_size = 0;
        }
};



#endif
