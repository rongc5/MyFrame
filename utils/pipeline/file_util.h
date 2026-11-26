#pragma once

#include "str_view.h"

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#if !defined(O_CLOEXEC) || !defined(O_DIRECTORY)
#error "myframe::pipeline::file_util requires POSIX.1-2008 (O_CLOEXEC and O_DIRECTORY)"
#endif

namespace myframe {
namespace pipeline {

static_assert(sizeof(off_t) == 8, "64-bit off_t is required for large file support");

namespace detail {

// Portable GNU/XSI strerror_r selection without relying on _GNU_SOURCE checks.
inline const char* se_pick(int rc, const char* buf) noexcept {
    return (rc == 0) ? buf : "unknown error";
}

inline const char* se_pick(const char* ptr, const char*) noexcept {
    return (ptr != nullptr) ? ptr : "unknown error";
}

inline std::string format_errno(const char* action, int err_num) {
    char buf[256];
    buf[0] = '\0';
    const char* msg = se_pick(::strerror_r(err_num, buf, sizeof(buf)), buf);
    return std::string(action) + ": " + msg;
}

inline void set_err(std::string* err, const std::string& msg) {
    if (err != nullptr) {
        *err = msg;
    }
}

inline std::uint64_t splitmix64(std::uint64_t x) noexcept {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

inline std::string hex64(std::uint64_t v) {
    static const char kHex[] = "0123456789abcdef";
    char out[16];
    for (int i = 15; i >= 0; --i) {
        out[i] = kHex[v & 0xfU];
        v >>= 4;
    }
    return std::string(out, 16);
}

inline int open_retry(const char* path, int flags, mode_t mode = 0) noexcept {
    int fd = -1;
    do {
        fd = ::open(path, flags, mode);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

inline ssize_t pread_retry(int fd, void* buf, std::size_t count, off_t offset) noexcept {
    ssize_t n = -1;
    do {
        n = ::pread(fd, buf, count, offset);
    } while (n < 0 && errno == EINTR);
    return n;
}

inline int fsync_retry(int fd) noexcept {
    int rc = -1;
    do {
        rc = ::fsync(fd);
    } while (rc < 0 && errno == EINTR);
    return rc;
}

inline bool write_all(int fd, const void* data, std::size_t len, int& out_errno) noexcept {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    std::size_t written = 0;
    while (written < len) {
        ssize_t n = -1;
        do {
            n = ::write(fd, p + written, len - written);
        } while (n < 0 && errno == EINTR);
        if (n < 0) {
            out_errno = errno;
            return false;
        }
        if (n == 0) {
            out_errno = EIO;
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    return true;
}

} // namespace detail

// RAII wrapper for a POSIX file descriptor.
// Never retries close() on EINTR (Linux releases the fd on the first close()).
class ScopedFd {
public:
    ScopedFd() noexcept : fd_(-1) {}
    explicit ScopedFd(int fd) noexcept : fd_(fd) {}

    ~ScopedFd() noexcept {
        reset();
    }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    ScopedFd(ScopedFd&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    int get() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }
    explicit operator bool() const noexcept { return valid(); }

    int release() noexcept {
        const int f = fd_;
        fd_ = -1;
        return f;
    }

    // Closes the descriptor and returns 0 on success, or errno on failure.
    int close_checked() noexcept {
        if (fd_ < 0) {
            return 0;
        }
        const int f = fd_;
        fd_ = -1;
        if (::close(f) != 0) {
            return errno;
        }
        return 0;
    }

    void reset(int new_fd = -1) noexcept {
        if (new_fd == fd_) {
            return;
        }
        if (fd_ >= 0) {
            (void)::close(fd_);
        }
        fd_ = new_fd;
    }

private:
    int fd_;
};

// Snapshot metadata for file change detection (e.g. hot-reload polling).
// Stores sec/nsec fields separately to avoid signed int64 overflow past year 2262,
// and captures ctime in addition to mtime so tools restoring mtime cannot mask edits.
struct FileStat {
    std::uint64_t size;
    std::int64_t mtime_sec;
    std::int64_t mtime_nsec;
    std::int64_t ctime_sec;
    std::int64_t ctime_nsec;
    std::uint64_t inode;
    std::uint64_t dev;
    bool is_regular;

    FileStat() noexcept
        : size(0),
          mtime_sec(0),
          mtime_nsec(0),
          ctime_sec(0),
          ctime_nsec(0),
          inode(0),
          dev(0),
          is_regular(false) {}

    bool operator==(const FileStat& other) const noexcept {
        return size == other.size &&
               mtime_sec == other.mtime_sec &&
               mtime_nsec == other.mtime_nsec &&
               ctime_sec == other.ctime_sec &&
               ctime_nsec == other.ctime_nsec &&
               inode == other.inode &&
               dev == other.dev &&
               is_regular == other.is_regular;
    }

    bool operator!=(const FileStat& other) const noexcept {
        return !(*this == other);
    }
};

namespace detail {

inline void populate_file_stat(const struct stat& st, FileStat& out) noexcept {
    out.size = static_cast<std::uint64_t>(st.st_size);
    out.mtime_sec = static_cast<std::int64_t>(st.st_mtim.tv_sec);
    out.mtime_nsec = static_cast<std::int64_t>(st.st_mtim.tv_nsec);
    out.ctime_sec = static_cast<std::int64_t>(st.st_ctim.tv_sec);
    out.ctime_nsec = static_cast<std::int64_t>(st.st_ctim.tv_nsec);
    out.inode = static_cast<std::uint64_t>(st.st_ino);
    out.dev = static_cast<std::uint64_t>(st.st_dev);
    out.is_regular = S_ISREG(st.st_mode);
}

} // namespace detail

inline bool stat_fd(int fd, FileStat& out, std::string* err = nullptr) {
    struct stat st;
    if (::fstat(fd, &st) != 0) {
        detail::set_err(err, detail::format_errno("fstat failed", errno));
        return false;
    }
    if (st.st_size < 0) {
        detail::set_err(err, "fstat returned negative file size");
        return false;
    }
    detail::populate_file_stat(st, out);
    return true;
}

inline bool stat_file(const std::string& path, FileStat& out, std::string* err = nullptr) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        detail::set_err(err, detail::format_errno("stat failed", errno));
        return false;
    }
    if (st.st_size < 0) {
        detail::set_err(err, "stat returned negative file size");
        return false;
    }
    detail::populate_file_stat(st, out);
    return true;
}

// Reads a regular file into `out` with an explicit upper bound `max_bytes`.
// Opens with O_NONBLOCK so accidentally pointing at a FIFO fails immediately
// at the S_ISREG check instead of blocking indefinitely. Clears `out` on error.
inline bool read_file(const std::string& path,
                      std::string& out,
                      std::size_t max_bytes = 64 * 1024 * 1024,
                      std::string* err = nullptr) {
    ScopedFd fd(detail::open_retry(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    if (!fd) {
        out.clear();
        detail::set_err(err, detail::format_errno("open failed", errno));
        return false;
    }
    FileStat st;
    if (!stat_fd(fd.get(), st, err)) {
        out.clear();
        return false;
    }
    if (!st.is_regular) {
        out.clear();
        detail::set_err(err, "read_file requires a regular file");
        return false;
    }
    if (st.size > static_cast<std::uint64_t>(max_bytes)) {
        out.clear();
        detail::set_err(err, "file exceeds max_bytes limit");
        return false;
    }

    std::string tmp;
    tmp.reserve(static_cast<std::size_t>(st.size));
    std::vector<char> buf(65536);
    std::uint64_t offset = 0;
    for (;;) {
        const ssize_t n = detail::pread_retry(
            fd.get(), &buf[0], buf.size(), static_cast<off_t>(offset));
        if (n < 0) {
            out.clear();
            detail::set_err(err, detail::format_errno("pread failed", errno));
            return false;
        }
        if (n == 0) {
            break;
        }
        const std::size_t got = static_cast<std::size_t>(n);
        if (got > max_bytes - tmp.size()) {
            out.clear();
            detail::set_err(err, "file grew beyond max_bytes limit while reading");
            return false;
        }
        tmp.append(&buf[0], got);
        offset += static_cast<std::uint64_t>(got);
    }

    out.swap(tmp);
    return true;
}

enum AtomicWriteStatus {
    kAtomicOk = 0,
    // rename() succeeded (new contents are visible), but syncing parent directory failed.
    kAtomicCommittedDirSyncFailed = 1,
    // Write failed before rename(); target file was not modified.
    kAtomicError = 2
};

// Crash-safe atomic file writer:
//   1. Creates a unique sibling temp file with O_CREAT|O_EXCL|O_CLOEXEC (no mkstemp fork race).
//   2. Writes full buffer (handling EINTR and short writes), fsync(fd), checked close(fd).
//   3. Atomically replaces `path` via rename() (replaces symlinks rather than following them).
//   4. Syncs parent directory (never unlinks after rename() succeeds).
//
// Note: ownership is not preserved. `mode` is subject to the process umask,
// whereas `preserve_mode = true` applies `fchmod(fd, st_mode & 07777)` (ignoring
// umask) when `path` already exists. The sibling temp name adds 22 bytes to the
// basename (basenames > 233 bytes may fail with ENAMETOOLONG).
// The `bool` overload returns false for BOTH kAtomicError and
// kAtomicCommittedDirSyncFailed; use write_file_atomic_ex() to distinguish them.
inline AtomicWriteStatus write_file_atomic_ex(const std::string& path,
                                              const void* data,
                                              std::size_t len,
                                              std::string* err = nullptr,
                                              mode_t mode = 0644,
                                              bool preserve_mode = false) {
    if (path.empty() || path.back() == '/') {
        detail::set_err(err, "write_file_atomic: invalid target path");
        return kAtomicError;
    }
    if (len > 0 && data == nullptr) {
        detail::set_err(err, "write_file_atomic: null data with non-zero length");
        return kAtomicError;
    }

    std::string dir;
    std::string base;
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        dir = ".";
        base = path;
    } else if (slash == 0) {
        dir = "/";
        base = path.substr(1);
    } else {
        dir = path.substr(0, slash);
        base = path.substr(slash + 1);
    }
    if (base.empty() || base == "." || base == "..") {
        detail::set_err(err, "write_file_atomic: invalid target filename");
        return kAtomicError;
    }

    bool have_existing_mode = false;
    mode_t existing_mode = mode;
    if (preserve_mode) {
        struct stat st;
        if (::stat(path.c_str(), &st) == 0) {
            existing_mode = static_cast<mode_t>(st.st_mode & 07777);
            have_existing_mode = true;
        }
    }

    static std::atomic<std::uint64_t> g_tmp_seq(1);
    const std::uint64_t pid_mix = static_cast<std::uint64_t>(::getpid());
    const std::uint64_t addr_mix = reinterpret_cast<std::uintptr_t>(&dir);
    std::string tmp_path;
    ScopedFd fd;

    for (int attempt = 0; attempt < 100; ++attempt) {
        const std::uint64_t seq = g_tmp_seq.fetch_add(1, std::memory_order_relaxed);
        const std::uint64_t now_ns = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const std::uint64_t token = detail::splitmix64(
            pid_mix ^ (seq * 0x9e3779b97f4a7c15ULL) ^ now_ns ^ addr_mix ^
            static_cast<std::uint64_t>(attempt));

        tmp_path = (dir == "/") ? ("/." + base + ".tmp." + detail::hex64(token))
                                : (dir + "/." + base + ".tmp." + detail::hex64(token));
        const int raw_fd = detail::open_retry(
            tmp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
        if (raw_fd >= 0) {
            fd.reset(raw_fd);
            break;
        }
        if (errno != EEXIST) {
            detail::set_err(err, detail::format_errno("open temp file failed", errno));
            return kAtomicError;
        }
    }
    if (!fd) {
        detail::set_err(err, "write_file_atomic: failed to allocate unique temp filename");
        return kAtomicError;
    }

    if (have_existing_mode) {
        if (::fchmod(fd.get(), existing_mode) != 0) {
            const int saved = errno;
            fd.reset();
            (void)::unlink(tmp_path.c_str());
            detail::set_err(err, detail::format_errno("fchmod failed", saved));
            return kAtomicError;
        }
    }

    int write_err = 0;
    if (len > 0 && !detail::write_all(fd.get(), data, len, write_err)) {
        fd.reset();
        (void)::unlink(tmp_path.c_str());
        detail::set_err(err, detail::format_errno("write failed", write_err));
        return kAtomicError;
    }

    if (detail::fsync_retry(fd.get()) != 0) {
        const int saved = errno;
        fd.reset();
        (void)::unlink(tmp_path.c_str());
        detail::set_err(err, detail::format_errno("fsync temp file failed", saved));
        return kAtomicError;
    }

    const int close_err = fd.close_checked();
    if (close_err != 0) {
        (void)::unlink(tmp_path.c_str());
        detail::set_err(err, detail::format_errno("close temp file failed", close_err));
        return kAtomicError;
    }

    if (::rename(tmp_path.c_str(), path.c_str()) != 0) {
        const int saved = errno;
        (void)::unlink(tmp_path.c_str());
        detail::set_err(err, detail::format_errno("rename failed", saved));
        return kAtomicError;
    }

    // Rename succeeded: NEVER unlink anything from this point on.
    ScopedFd dir_fd(detail::open_retry(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!dir_fd) {
        detail::set_err(err, detail::format_errno("open parent dir for fsync failed", errno));
        return kAtomicCommittedDirSyncFailed;
    }
    if (detail::fsync_retry(dir_fd.get()) != 0) {
        const int saved = errno;
        if (saved != EINVAL && saved != ENOTSUP && saved != EROFS) {
            detail::set_err(err, detail::format_errno("fsync parent dir failed", saved));
            return kAtomicCommittedDirSyncFailed;
        }
    }
    return kAtomicOk;
}

inline bool write_file_atomic(const std::string& path,
                              const void* data,
                              std::size_t len,
                              std::string* err = nullptr,
                              mode_t mode = 0644,
                              bool preserve_mode = false) {
    return write_file_atomic_ex(path, data, len, err, mode, preserve_mode) == kAtomicOk;
}

inline bool write_file_atomic(const std::string& path,
                              const std::string& content,
                              std::string* err = nullptr,
                              mode_t mode = 0644,
                              bool preserve_mode = false) {
    return write_file_atomic(
        path, content.empty() ? nullptr : content.data(), content.size(), err, mode, preserve_mode);
}

// Half-open newline-aligned byte range [start, end).
struct ByteRange {
    std::uint64_t start;
    std::uint64_t end;
};

namespace detail {

// Finds the smallest byte offset `nl >= search_from` in `[search_from, file_size)`
// whose byte is '\n'. Writes `nl + 1` to `out_boundary`, or `file_size` if no '\n'
// exists before `file_size`. Returns false on pread error or unexpected truncation.
inline bool find_next_line_start(int fd,
                                 std::uint64_t search_from,
                                 std::uint64_t file_size,
                                 std::uint64_t& out_boundary,
                                 std::string* err) {
    std::vector<char> buf(65536);
    std::uint64_t pos = search_from;
    while (pos < file_size) {
        const std::uint64_t rem = file_size - pos;
        const std::size_t to_read =
            (rem < buf.size()) ? static_cast<std::size_t>(rem) : buf.size();
        const ssize_t n = pread_retry(fd, &buf[0], to_read, static_cast<off_t>(pos));
        if (n < 0) {
            set_err(err, format_errno("pread failed during split", errno));
            return false;
        }
        if (n == 0) {
            set_err(err, "file truncated during split");
            return false;
        }
        const std::size_t got = static_cast<std::size_t>(n);
        const void* nl = std::memchr(&buf[0], '\n', got);
        if (nl != nullptr) {
            const std::size_t delta = static_cast<std::size_t>(
                static_cast<const char*>(nl) - &buf[0]);
            out_boundary = pos + static_cast<std::uint64_t>(delta) + 1ULL;
            return true;
        }
        pos += static_cast<std::uint64_t>(got);
    }
    out_boundary = file_size;
    return true;
}

} // namespace detail

// Splits `[0, file_size)` of an already-open read-only descriptor `fd` into up to
// `num_chunks` contiguous, non-empty, newline-aligned `[start, end)` ranges.
//
// Invariants on success:
//   - Every range has `start < end`.
//   - Every `start` is a line start (0 or preceded by '\n'), so CRLF is never split.
//   - Giant lines spanning multiple target offsets are scanned only once (O(file_size))
//     and empty ranges are deduplicated (`out_ranges.size() <= num_chunks`).
inline bool split_fd_by_lines(int fd,
                              std::uint64_t file_size,
                              std::size_t num_chunks,
                              std::vector<ByteRange>& out_ranges,
                              std::string* err = nullptr) {
    out_ranges.clear();
    if (file_size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        detail::set_err(err, "file_size exceeds off_t range");
        return false;
    }
    if (file_size == 0) {
        return true;
    }

    static const std::size_t kMaxChunks = 1048576; // 1M cap keeps r*idx overflow-free
    std::size_t n = (num_chunks == 0) ? 1 : num_chunks;
    if (n > kMaxChunks) {
        n = kMaxChunks;
    }
    if (static_cast<std::uint64_t>(n) > file_size) {
        n = static_cast<std::size_t>(file_size);
    }
    if (n == 1) {
        out_ranges.push_back(ByteRange{0, file_size});
        return true;
    }

    out_ranges.reserve(n);
    const std::uint64_t q = file_size / static_cast<std::uint64_t>(n);
    const std::uint64_t r = file_size % static_cast<std::uint64_t>(n);

    std::uint64_t prev = 0;
    for (std::size_t i = 1; i < n && prev < file_size; ++i) {
        const std::uint64_t idx = static_cast<std::uint64_t>(i);
        // Overflow-free exact floor(file_size * i / n) since r < n <= 2^20.
        const std::uint64_t target = q * idx + (r * idx) / static_cast<std::uint64_t>(n);
        if (target <= prev) {
            continue;
        }
        std::uint64_t boundary = file_size;
        if (!detail::find_next_line_start(fd, target - 1ULL, file_size, boundary, err)) {
            out_ranges.clear();
            return false;
        }
        if (boundary > prev) {
            out_ranges.push_back(ByteRange{prev, boundary});
            prev = boundary;
        }
    }
    if (file_size > prev) {
        out_ranges.push_back(ByteRange{prev, file_size});
    }

#ifndef NDEBUG
    assert(!out_ranges.empty());
    assert(out_ranges.front().start == 0);
    assert(out_ranges.back().end == file_size);
    for (std::size_t i = 0; i < out_ranges.size(); ++i) {
        assert(out_ranges[i].start < out_ranges[i].end);
        if (i > 0) {
            assert(out_ranges[i].start == out_ranges[i - 1].end);
        }
    }
#endif
    return true;
}

inline bool split_file_by_lines(const std::string& path,
                                std::size_t num_chunks,
                                std::vector<ByteRange>& out_ranges,
                                std::string* err = nullptr) {
    out_ranges.clear();
    ScopedFd fd(detail::open_retry(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    if (!fd) {
        detail::set_err(err, detail::format_errno("open failed", errno));
        return false;
    }
    FileStat st;
    if (!stat_fd(fd.get(), st, err)) {
        return false;
    }
    if (!st.is_regular) {
        detail::set_err(err, "split_file_by_lines requires a regular file");
        return false;
    }
    return split_fd_by_lines(fd.get(), st.size, num_chunks, out_ranges, err);
}

// High-throughput pread-based buffered line reader over a newline-aligned byte
// range `[start_offset, end_offset)` of a shared read-only file descriptor.
//
// Contract:
//   - Does NOT own `fd`; caller keeps `fd` open for the reader's lifetime.
//   - Returned StringView in next() is valid ONLY until the next call to next().
//   - Strips a single trailing '\r' ONLY when the line was terminated by '\n'
//     (handles '\r' at the end of a read buffer and '\n' at byte 0 of the next read).
//   - Embedded NUL bytes are preserved faithfully.
//   - `max_line_bytes` counts raw bytes excluding '\n' (including any trailing '\r');
//     0 is clamped to 1. Lines exceeding the limit never grow memory past
//     `buf_size + max_line_bytes`, discard the remainder up to '\n', and return
//     kTooLong with line_offset() set to the line's starting offset.
class BufferedLineReader {
public:
    enum Status {
        kLine = 0,
        kEof = 1,
        kTooLong = 2,
        kError = 3
    };

    BufferedLineReader(int fd,
                       std::uint64_t start_offset,
                       std::uint64_t end_offset,
                       std::uint64_t file_size,
                       std::size_t max_line_bytes = 16 * 1024 * 1024,
                       std::size_t buf_size = 65536)
        : fd_(fd),
          pos_(start_offset),
          end_(end_offset),
          file_size_(file_size),
          max_line_bytes_(max_line_bytes == 0 ? 1 : max_line_bytes),
          buf_(buf_size < 64 ? 64 : buf_size),
          buf_pos_(0),
          buf_len_(0),
          carry_(),
          line_offset_(start_offset),
          error_(),
          last_errno_(0),
          bad_init_(false) {
        if (fd_ < 0 ||
            start_offset > end_offset ||
            end_offset > file_size ||
            file_size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
            bad_init_ = true;
            error_ = "BufferedLineReader invalid fd or byte range";
        }
    }

    BufferedLineReader(const BufferedLineReader&) = delete;
    BufferedLineReader& operator=(const BufferedLineReader&) = delete;

    Status next(StringView& out_line) {
        out_line = StringView();
        if (bad_init_) {
            return kError;
        }

        // Current absolute offset of the next unread byte.
        line_offset_ = current_offset();

        // Fast path: line fits completely inside the current buffer.
        if (buf_pos_ < buf_len_) {
            const std::size_t avail = buf_len_ - buf_pos_;
            const void* nl = std::memchr(&buf_[buf_pos_], '\n', avail);
            if (nl != nullptr) {
                const std::size_t raw_len = static_cast<std::size_t>(
                    static_cast<const char*>(nl) - &buf_[buf_pos_]);
                const char* line_start = &buf_[buf_pos_];
                buf_pos_ += raw_len + 1;
                if (raw_len > max_line_bytes_) {
                    return kTooLong;
                }
                const std::size_t trimmed_len =
                    (raw_len > 0 && line_start[raw_len - 1] == '\r') ? (raw_len - 1) : raw_len;
                out_line = StringView(line_start, trimmed_len);
                return kLine;
            }
        }

        // Slow path: accumulate across refills (or handle EOF of range).
        carry_.clear();
        bool has_any_bytes = false;
        bool too_long = false;

        for (;;) {
            if (buf_pos_ < buf_len_) {
                has_any_bytes = true;
                const std::size_t avail = buf_len_ - buf_pos_;
                const void* nl = std::memchr(&buf_[buf_pos_], '\n', avail);
                if (nl != nullptr) {
                    const std::size_t seg_len = static_cast<std::size_t>(
                        static_cast<const char*>(nl) - &buf_[buf_pos_]);
                    if (!too_long) {
                        if (seg_len > max_line_bytes_ - carry_.size()) {
                            too_long = true;
                            carry_.clear();
                        } else {
                            carry_.append(&buf_[buf_pos_], seg_len);
                        }
                    }
                    buf_pos_ += seg_len + 1;
                    if (too_long) {
                        return kTooLong;
                    }
                    if (!carry_.empty() && carry_.back() == '\r') {
                        carry_.pop_back();
                    }
                    out_line = StringView(carry_);
                    return kLine;
                }

                // No '\n' in remaining buffer bytes.
                if (!too_long) {
                    if (avail > max_line_bytes_ - carry_.size()) {
                        too_long = true;
                        carry_.clear();
                    } else {
                        carry_.append(&buf_[buf_pos_], avail);
                    }
                }
                buf_pos_ = buf_len_;
            }

            // Refill buffer up to end_.
            if (pos_ >= end_) {
                if (!has_any_bytes) {
                    return kEof;
                }
                // Reached end_ without seeing '\n'. Valid ONLY for the final chunk.
                if (end_ != file_size_) {
                    error_ = "file modified during read: non-final chunk missing trailing newline";
                    return kError;
                }
                if (too_long) {
                    return kTooLong;
                }
                // Per policy: do NOT strip trailing '\r' when there is no '\n'.
                out_line = StringView(carry_);
                return kLine;
            }

            const std::uint64_t rem = end_ - pos_;
            const std::size_t to_read =
                (rem < buf_.size()) ? static_cast<std::size_t>(rem) : buf_.size();
            const ssize_t n = detail::pread_retry(
                fd_, &buf_[0], to_read, static_cast<off_t>(pos_));
            if (n < 0) {
                last_errno_ = errno;
                error_ = detail::format_errno("pread failed", last_errno_);
                return kError;
            }
            if (n == 0) {
                error_ = "file truncated during read";
                return kError;
            }
            buf_pos_ = 0;
            buf_len_ = static_cast<std::size_t>(n);
            pos_ += static_cast<std::uint64_t>(buf_len_);
        }
    }

    std::uint64_t line_offset() const noexcept { return line_offset_; }
    const std::string& error() const noexcept { return error_; }
    int last_errno() const noexcept { return last_errno_; }

private:
    std::uint64_t current_offset() const noexcept {
        return pos_ - static_cast<std::uint64_t>(buf_len_ - buf_pos_);
    }

    int fd_;
    std::uint64_t pos_;
    std::uint64_t end_;
    std::uint64_t file_size_;
    std::size_t max_line_bytes_;
    std::vector<char> buf_;
    std::size_t buf_pos_;
    std::size_t buf_len_;
    std::string carry_;
    std::uint64_t line_offset_;
    std::string error_;
    int last_errno_;
    bool bad_init_;
};

} // namespace pipeline
} // namespace myframe
