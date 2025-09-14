#pragma once
#include <string>
#include <vector>
#include <cstdlib>

namespace myframe {

inline size_t pool_capacity() {
    static size_t cap = []{
        const char* e = std::getenv("MYFRAME_STRPOOL_CAP");
        long v = e ? std::atol(e) : 256;
        if (v <= 0) v = 256; if (v > 4096) v = 4096;
        return (size_t)v;
    }();
    return cap;
}

inline std::string* string_acquire() {
    thread_local std::vector<std::string*> pool;
    if (!pool.empty()) {
        std::string* s = pool.back(); pool.pop_back(); s->clear(); return s;
    }
    return new std::string();
}

inline void string_release(std::string* s) {
    if (!s) return;
    thread_local std::vector<std::string*> pool;
    if (pool.size() < pool_capacity()) { s->clear(); pool.push_back(s); }
    else { delete s; }
}

} // namespace myframe

