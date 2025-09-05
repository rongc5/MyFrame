// Minimal, self-contained logging compatibility header
#pragma once

#include "base_def.h"
#include <pthread.h>

// Map legacy LOG_* macros to PDEBUG (or no-op) to avoid pulling legacy logging system.

#ifndef PDEBUG
#define PDEBUG(format, ...) do { /* define PDEBUG in base_def.h for debug prints */ } while(0)
#endif

#define LOG_INIT_OLD(log_conf) do { (void)(log_conf); } while(0)
#define LOG_INIT(log_path)     do { (void)(log_path); } while(0)

#define LOG_WARNING(fmt, ...)  do { PDEBUG(fmt, ##__VA_ARGS__); } while(0)
#define LOG_FATAL(fmt, ...)    do { PDEBUG(fmt, ##__VA_ARGS__); } while(0)
#define LOG_NOTICE(fmt, ...)   do { PDEBUG(fmt, ##__VA_ARGS__); } while(0)
#define LOG_TRACE(fmt, ...)    do { PDEBUG(fmt, ##__VA_ARGS__); } while(0)
#define LOG_DEBUG(fmt, ...)    do { PDEBUG(fmt, ##__VA_ARGS__); } while(0)

// Legacy stream-like macros become no-ops
struct __log_stream_noop { template<typename T> __log_stream_noop& operator<<(const T&) { return *this; } };
#define LOGWARNING (__log_stream_noop())
#define LOGFATAL   (__log_stream_noop())
#define LOGNOTICE  (__log_stream_noop())
#define LOGTRACE   (__log_stream_noop())
#define LOGDEBUG   (__log_stream_noop())

#define LOG_WRITE(t, fmt, ...) do { (void)(t); PDEBUG(fmt, ##__VA_ARGS__); } while(0)
#define FILE_WRITE(f, fmt, ...) do { (void)(f); PDEBUG(fmt, ##__VA_ARGS__); } while(0)
