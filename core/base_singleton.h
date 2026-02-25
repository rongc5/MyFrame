#ifndef __BASE_SINGLETON_H__
#define __BASE_SINGLETON_H__

#include <mutex>
#include <memory>

template<class T>
class base_singleton
{
public:
	// 返回当前实例的 shared_ptr（调用者持有期间保证对象不被释放）
	static std::shared_ptr<T> get_instance()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _p_instance;
	}

    // 延迟创建并返回实例（线程安全）
    static std::shared_ptr<T> get_instance_ex()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_p_instance){
            _p_instance = std::make_shared<T>();
        }
        return _p_instance;
    }

	// 推荐：使用 shared_ptr 替换实例，所有权语义明确
	static void set_instance(std::shared_ptr<T> instance)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_p_instance = std::move(instance);
	}

	// 已废弃：请使用 set_instance(std::shared_ptr<T>) 版本
#if __cplusplus >= 201402L
	[[deprecated("use set_instance(std::shared_ptr<T>) instead")]]
#elif defined(__GNUC__) || defined(__clang__)
	__attribute__((deprecated("use set_instance(std::shared_ptr<T>) instead")))
#endif
	static void set_instance(T *instance)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_p_instance.reset(instance);
	}
private:
	static std::shared_ptr<T> _p_instance;
	static std::mutex _mutex;
};

template<class T>
std::shared_ptr<T> base_singleton<T>::_p_instance;

template<class T>
std::mutex base_singleton<T>::_mutex;

#endif
