#pragma once
#include <string>
#include <vector>
#include <cstdlib>
#include <memory>
#include <cstring>
#include <strings.h>

namespace myframe {

inline size_t pool_capacity() {
    static size_t cap = []{
        const char* e = std::getenv("MYFRAME_STRPOOL_CAP");
        long v = 0;
        if (e) v = std::atol(e);
        if (v <= 0) {
            const char* preset = std::getenv("MYFRAME_PERF_PRESET");
            if (preset && (strcmp(preset, "0") != 0 && strcasecmp(preset, "false") != 0)) v = 512;
        }
        if (v <= 0) v = 256; if (v > 4096) v = 4096;
        return (size_t)v;
    }();
    return cap;
}

constexpr size_t kPooledStringMaxCapacity = 64 * 1024;

namespace detail {
// One pool per thread, shared by string_acquire() and string_release(). The
// pool frees its cached strings when the thread exits; a trivially
// destructible flag lets late releases (during TLS teardown) fall back to a
// plain delete instead of touching the destroyed pool.
struct string_pool_tls {
    std::vector<std::string*> items;
    string_pool_tls();
    ~string_pool_tls();
};

inline bool& string_pool_alive() {
    thread_local bool alive = false;
    return alive;
}

inline string_pool_tls::string_pool_tls() { string_pool_alive() = true; }

inline string_pool_tls::~string_pool_tls() {
    string_pool_alive() = false;
    for (std::string* s : items) delete s;
    items.clear();
}

inline string_pool_tls* string_pool_get() {
    thread_local string_pool_tls pool;
    return string_pool_alive() ? &pool : nullptr;
}
} // namespace detail

inline std::string* string_acquire() {
    detail::string_pool_tls* pool = detail::string_pool_get();
    if (pool && !pool->items.empty()) {
        std::string* s = pool->items.back(); pool->items.pop_back(); s->clear(); return s;
    }
    return new std::string();
}

inline void string_release(std::string* s) {
    if (!s) return;
    detail::string_pool_tls* pool = detail::string_pool_get();
    // Do not cache very large buffers: their capacity would stay resident.
    if (pool && pool->items.size() < pool_capacity() && s->capacity() <= kPooledStringMaxCapacity) {
        s->clear(); pool->items.push_back(s);
    }
    else { delete s; }
}

struct string_releaser {
    void operator()(std::string* s) const noexcept {
        string_release(s);
    }
};

using pooled_string_ptr = std::unique_ptr<std::string, string_releaser>;

inline pooled_string_ptr make_pooled_string(std::string* s) {
    return pooled_string_ptr(s);
}

} // namespace myframe
