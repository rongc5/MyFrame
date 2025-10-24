#pragma once

#include <pthread.h>
#include <stdexcept>

// C++11 下基于 pthread 的读写锁 RAII 封装
// 适用于读多写少的场景，多个读者可以并发访问，写者独占

class RWLock {
public:
    RWLock() {
        int ret = pthread_rwlock_init(&_lock, nullptr);
        if (ret != 0) {
            throw std::runtime_error("pthread_rwlock_init failed");
        }
    }

    ~RWLock() {
        pthread_rwlock_destroy(&_lock);
    }

    void lock_read() {
        int ret = pthread_rwlock_rdlock(&_lock);
        if (ret != 0) {
            throw std::runtime_error("pthread_rwlock_rdlock failed");
        }
    }

    void unlock_read() {
        pthread_rwlock_unlock(&_lock);
    }

    void lock_write() {
        int ret = pthread_rwlock_wrlock(&_lock);
        if (ret != 0) {
            throw std::runtime_error("pthread_rwlock_wrlock failed");
        }
    }

    void unlock_write() {
        pthread_rwlock_unlock(&_lock);
    }

private:
    pthread_rwlock_t _lock;

    RWLock(const RWLock&) = delete;
    RWLock& operator=(const RWLock&) = delete;
};

// RAII 读锁守卫
class ReadLockGuard {
public:
    explicit ReadLockGuard(RWLock& lock) : _lock(lock) {
        _lock.lock_read();
    }

    ~ReadLockGuard() {
        _lock.unlock_read();
    }

private:
    RWLock& _lock;
    ReadLockGuard(const ReadLockGuard&) = delete;
    ReadLockGuard& operator=(const ReadLockGuard&) = delete;
};

// RAII 写锁守卫
class WriteLockGuard {
public:
    explicit WriteLockGuard(RWLock& lock) : _lock(lock) {
        _lock.lock_write();
    }

    ~WriteLockGuard() {
        _lock.unlock_write();
    }

private:
    RWLock& _lock;
    WriteLockGuard(const WriteLockGuard&) = delete;
    WriteLockGuard& operator=(const WriteLockGuard&) = delete;
};
