#ifndef __BASE_RELOAD_H_
#define __BASE_RELOAD_H_

#include "base_def.h"
#include <atomic>
#include <cstdint>

/**
 * @brief 需要提供Load、Reload功能的类型，需要继承此接口。
 * 在ReloadMgr管理时需要调用Load、Reload方法
 * */
class reload_inf
{
    public:
        virtual ~reload_inf() {}
        virtual int load() = 0;
        virtual int reload() = 0;
        virtual bool need_reload()=0;
        virtual int dump() = 0;
        virtual int destroy() = 0;
};

/**
 *@brief 重载管理器，判断是否需要重载
 *
 * 线程安全说明：
 * - _curr 使用 atomic + acquire/release 保证跨线程可见性
 * - 写线程：调用 reload()，在 idle 对象上重载后原子切换
 * - 读线程：调用 current()，获取当前有效对象
 */
template<typename T>
class reload_mgr: public reload_inf
{
    public:
        reload_mgr(T * T1, T *T2);
        virtual ~reload_mgr();

        /**
         *@brief 加载数据
         */
        int load();

        /**
         * @brief 重载数据
         */
        int reload();

        T* current();

        bool need_reload();

        int dump();

        int destroy();

        T* idle();

    private:
        T * _objects[2];
        std::atomic<int16_t> _curr;
};

template<typename T>
reload_mgr<T>::reload_mgr(T * T1, T *T2)
{
    _objects[0] = T1;
    _objects[1] = T2;
    _curr.store(0, std::memory_order_relaxed);
}

template<typename T>
reload_mgr<T>::~reload_mgr()
{
    if (_objects[0])
    {
        _objects[0]->destroy();
        delete _objects[0];
        _objects[0] = NULL;
    }

    if (_objects[1])
    {
        _objects[1]->destroy();
        delete _objects[1];
        _objects[1] = NULL;
    }
}

/**
 *@brief 加载数据
 */
template<typename T>
int reload_mgr<T>::load()
{
    int16_t c = _curr.load(std::memory_order_acquire);
    if( _objects[c]->load() == 0 )
    {
        return 0;
    }

    return -1;
}

template<typename T>
bool reload_mgr<T>::need_reload()
{
    return current()->need_reload();
}


/**
 * @brief 重载数据
 */
template<typename T>
int reload_mgr<T>::reload()
{
    int16_t c = _curr.load(std::memory_order_relaxed);
    if ( _objects[1 - c]->reload() == 0 )
    {
        _curr.store(1 - c, std::memory_order_release);
        return 0;
    } else
    {
        PDEBUG("reload data failed,%d", c);
        return -1;
    }

    return 0;
}

template<typename T>
T* reload_mgr<T>::current() {
    int16_t c = _curr.load(std::memory_order_acquire);
    if( c == 0 || c == 1){
        return (_objects[c]);
    }

    return NULL;
}

template<typename T>
int reload_mgr<T>::dump()
{
    reload_inf* obj = current();
    return obj->dump();
}

template<typename T>
int reload_mgr<T>::destroy()
{
    if (_objects[0])
        _objects[0]->destroy();

    if (_objects[1])
        _objects[1]->destroy();

    return 0;
}

template<typename T>
T* reload_mgr<T>::idle()
{
    int16_t c = _curr.load(std::memory_order_acquire);
    if( c == 0 || c == 1){
        return (_objects[1 - c]);
    }

    return NULL;
}

#endif
