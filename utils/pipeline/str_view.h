#pragma once

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <locale.h>
#include <string>
#include <vector>

namespace myframe {
namespace pipeline {

// Namespace-scope sentinel avoids C++11 ODR-use multiple-definition link errors
// that occur with `static constexpr` class members in header-only classes.
static const std::size_t kNpos = ~std::size_t(0);

// Lightweight C++11 non-owning byte-slice view (UTF-8 and embedded-NUL safe).
//
// Invariants:
//   - `(nullptr, 0)` is valid; memcmp/memchr/memcpy/std::string are never called
//     with a null pointer when size == 0.
//   - Never throws on substr() (out-of-range pos clamps to empty view).
//   - Non-owning: like std::string_view, the underlying buffer / std::string must
//     outlive the StringView (do not bind to a temporary std::string).
class StringView {
public:
    StringView() noexcept : data_(nullptr), size_(0) {}

    StringView(const char* data, std::size_t len) noexcept
        : data_(len == 0 ? nullptr : data), size_(len) {}

    StringView(const char* cstr) noexcept
        : data_(cstr), size_(cstr != nullptr ? std::strlen(cstr) : 0) {}

    StringView(const std::string& s) noexcept
        : data_(s.empty() ? nullptr : s.data()), size_(s.size()) {}

    const char* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    char operator[](std::size_t idx) const noexcept { return data_[idx]; }
    char front() const noexcept { return data_[0]; }
    char back() const noexcept { return data_[size_ - 1]; }

    const char* begin() const noexcept { return data_; }
    const char* end() const noexcept { return size_ == 0 ? data_ : (data_ + size_); }

    void remove_prefix(std::size_t n) noexcept {
        if (n >= size_) {
            data_ = nullptr;
            size_ = 0;
        } else {
            data_ += n;
            size_ -= n;
        }
    }

    void remove_suffix(std::size_t n) noexcept {
        if (n >= size_) {
            data_ = nullptr;
            size_ = 0;
        } else {
            size_ -= n;
        }
    }

    // Clamps `pos > size_` to empty view (never throws).
    StringView substr(std::size_t pos, std::size_t count = kNpos) const noexcept {
        if (pos >= size_) {
            return StringView();
        }
        const std::size_t avail = size_ - pos;
        const std::size_t len = (count < avail) ? count : avail;
        return StringView(data_ + pos, len);
    }

    std::size_t find(char ch, std::size_t pos = 0) const noexcept {
        if (pos >= size_ || data_ == nullptr) {
            return kNpos;
        }
        const void* found = std::memchr(data_ + pos, static_cast<unsigned char>(ch), size_ - pos);
        if (found == nullptr) {
            return kNpos;
        }
        return static_cast<std::size_t>(static_cast<const char*>(found) - data_);
    }

    bool starts_with(StringView prefix) const noexcept {
        if (prefix.size_ > size_) {
            return false;
        }
        if (prefix.size_ == 0) {
            return true;
        }
        return std::memcmp(data_, prefix.data_, prefix.size_) == 0;
    }

    bool ends_with(StringView suffix) const noexcept {
        if (suffix.size_ > size_) {
            return false;
        }
        if (suffix.size_ == 0) {
            return true;
        }
        return std::memcmp(data_ + (size_ - suffix.size_), suffix.data_, suffix.size_) == 0;
    }

    // Strips ASCII whitespace (' ', '\t', '\n', '\v', '\f', '\r').
    // Does not call std::isspace (avoids locale dependence and negative-char UB).
    StringView trim() const noexcept {
        std::size_t b = 0;
        std::size_t e = size_;
        while (b < e && is_ascii_space(data_[b])) {
            ++b;
        }
        while (e > b && is_ascii_space(data_[e - 1])) {
            --e;
        }
        return (e == b) ? StringView() : StringView(data_ + b, e - b);
    }

    StringView trim_crlf() const noexcept {
        std::size_t e = size_;
        while (e > 0 && (data_[e - 1] == '\r' || data_[e - 1] == '\n')) {
            --e;
        }
        return (e == 0) ? StringView() : StringView(data_, e);
    }

    std::string to_string() const {
        if (size_ == 0 || data_ == nullptr) {
            return std::string();
        }
        return std::string(data_, size_);
    }

    int compare(StringView other) const noexcept {
        const std::size_t min_len = (size_ < other.size_) ? size_ : other.size_;
        if (min_len > 0) {
            const int cmp = std::memcmp(data_, other.data_, min_len);
            if (cmp != 0) {
                return cmp;
            }
        }
        if (size_ < other.size_) return -1;
        if (size_ > other.size_) return 1;
        return 0;
    }

    friend bool operator==(StringView a, StringView b) noexcept {
        if (a.size_ != b.size_) {
            return false;
        }
        if (a.size_ == 0) {
            return true;
        }
        return std::memcmp(a.data_, b.data_, a.size_) == 0;
    }

    friend bool operator!=(StringView a, StringView b) noexcept { return !(a == b); }
    friend bool operator<(StringView a, StringView b) noexcept { return a.compare(b) < 0; }
    friend bool operator<=(StringView a, StringView b) noexcept { return a.compare(b) <= 0; }
    friend bool operator>(StringView a, StringView b) noexcept { return a.compare(b) > 0; }
    friend bool operator>=(StringView a, StringView b) noexcept { return a.compare(b) >= 0; }

private:
    static bool is_ascii_space(char c) noexcept {
        return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
    }

    const char* data_;
    std::size_t size_;
};

// -----------------------------------------------------------------------------
// Zero-allocation delimiter splitting
//
// Exact semantics:
//   - ""    -> 1 empty field [""]
//   - ","   -> 2 empty fields ["", ""]
//   - "a,"  -> 2 fields ["a", ""]
//   - "a,b" -> 2 fields ["a", "b"]
// -----------------------------------------------------------------------------

// Calls `fn(StringView field)` until `fn` returns false or all fields are visited.
template <typename Fn>
void split_until(StringView input, char delim, Fn&& fn) {
    std::size_t start = 0;
    for (;;) {
        const std::size_t pos = input.find(delim, start);
        if (pos == kNpos) {
            (void)fn(input.substr(start));
            return;
        }
        if (!fn(input.substr(start, pos - start))) {
            return;
        }
        start = pos + 1;
    }
}

// Visits every field calling `fn(StringView field)` (void callback).
template <typename Fn>
void split(StringView input, char delim, Fn&& fn) {
    split_until(input, delim, [&fn](StringView field) -> bool {
        fn(field);
        return true;
    });
}

// Clears `out` (reusing its existing capacity) and populates it with all fields.
inline void split_into(StringView input, char delim, std::vector<StringView>& out) {
    out.clear();
    split(input, delim, [&out](StringView field) {
        out.push_back(field);
    });
}

// -----------------------------------------------------------------------------
// Strict, locale-independent numeric parsing for StringView
//
// Rules shared across parse_i64 / parse_u64 / parse_double:
//   - Rejects empty input, leading/trailing whitespace, embedded NUL, trailing junk.
//   - Leading '+' is accepted consistently across all three parsers.
//   - Does not modify `out` on failure.
// -----------------------------------------------------------------------------

inline bool parse_u64(StringView sv, std::uint64_t& out) noexcept {
    if (sv.empty()) {
        return false;
    }
    std::size_t i = 0;
    if (sv[0] == '+') {
        ++i;
        if (i == sv.size()) {
            return false;
        }
    }
    std::uint64_t acc = 0;
    static const std::uint64_t kMaxDiv10 = std::numeric_limits<std::uint64_t>::max() / 10;
    static const std::uint64_t kMaxMod10 = std::numeric_limits<std::uint64_t>::max() % 10;

    for (; i < sv.size(); ++i) {
        const char ch = sv[i];
        if (ch < '0' || ch > '9') {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
        if (acc > kMaxDiv10 || (acc == kMaxDiv10 && digit > kMaxMod10)) {
            return false;
        }
        acc = acc * 10 + digit;
    }
    out = acc;
    return true;
}

inline bool parse_i64(StringView sv, std::int64_t& out) noexcept {
    if (sv.empty()) {
        return false;
    }
    std::size_t i = 0;
    bool neg = false;
    if (sv[0] == '-') {
        neg = true;
        ++i;
    } else if (sv[0] == '+') {
        ++i;
    }
    if (i == sv.size()) {
        return false;
    }

    // Accumulate magnitude in uint64_t so INT64_MIN (-9223372036854775808) is
    // parsed without signed overflow UB.
    const std::uint64_t limit = neg
        ? (static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1ULL)
        : static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const std::uint64_t limit_div10 = limit / 10;
    const std::uint64_t limit_mod10 = limit % 10;

    std::uint64_t acc = 0;
    for (; i < sv.size(); ++i) {
        const char ch = sv[i];
        if (ch < '0' || ch > '9') {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
        if (acc > limit_div10 || (acc == limit_div10 && digit > limit_mod10)) {
            return false;
        }
        acc = acc * 10 + digit;
    }

    if (neg) {
        if (acc == limit) {
            out = std::numeric_limits<std::int64_t>::min();
        } else {
            out = -static_cast<std::int64_t>(acc);
        }
    } else {
        out = static_cast<std::int64_t>(acc);
    }
    return true;
}

namespace detail {

inline locale_t cached_c_locale() noexcept {
    // Thread-safe one-time initialization in C++11; intentionally never freed.
    static locale_t c_loc = ::newlocale(LC_ALL_MASK, "C", static_cast<locale_t>(0));
    return c_loc;
}

class ScopedCLocale {
public:
    ScopedCLocale() noexcept : prev_(static_cast<locale_t>(0)), active_(false) {
        locale_t c_loc = cached_c_locale();
        if (c_loc != static_cast<locale_t>(0)) {
            prev_ = ::uselocale(c_loc);
            active_ = (prev_ != static_cast<locale_t>(0));
        }
    }

    ~ScopedCLocale() noexcept {
        if (active_) {
            (void)::uselocale(prev_);
        }
    }

    ScopedCLocale(const ScopedCLocale&) = delete;
    ScopedCLocale& operator=(const ScopedCLocale&) = delete;

private:
    locale_t prev_;
    bool active_;
};

// Strict decimal float grammar: [+-]?(digits(\.digits?)? | \.digits)([eE][+-]?digits)?
// Rejects leading/trailing whitespace, hex floats, NaN, Inf, and embedded NUL.
inline bool is_valid_decimal_float(StringView sv) noexcept {
    if (sv.empty()) {
        return false;
    }
    std::size_t i = 0;
    if (sv[i] == '+' || sv[i] == '-') {
        ++i;
        if (i == sv.size()) {
            return false;
        }
    }
    bool has_digits = false;
    while (i < sv.size() && sv[i] >= '0' && sv[i] <= '9') {
        has_digits = true;
        ++i;
    }
    if (i < sv.size() && sv[i] == '.') {
        ++i;
        while (i < sv.size() && sv[i] >= '0' && sv[i] <= '9') {
            has_digits = true;
            ++i;
        }
    }
    if (!has_digits) {
        return false;
    }
    if (i < sv.size() && (sv[i] == 'e' || sv[i] == 'E')) {
        ++i;
        if (i < sv.size() && (sv[i] == '+' || sv[i] == '-')) {
            ++i;
        }
        bool has_exp_digits = false;
        while (i < sv.size() && sv[i] >= '0' && sv[i] <= '9') {
            has_exp_digits = true;
            ++i;
        }
        if (!has_exp_digits) {
            return false;
        }
    }
    return i == sv.size();
}

} // namespace detail

// Parses a strict decimal floating-point value under the "C" locale.
// Preserves the caller's `errno`. Accepts finite subnormals and underflow to +/-0.0;
// rejects overflow (+/-HUGE_VAL), NaN, Inf, hex floats, whitespace, and trailing characters.
inline bool parse_double(StringView sv, double& out) {
    if (!detail::is_valid_decimal_float(sv)) {
        return false;
    }

    char stack_buf[128];
    std::string heap_buf;
    const char* cstr = nullptr;
    if (sv.size() < sizeof(stack_buf)) {
        std::memcpy(stack_buf, sv.data(), sv.size());
        stack_buf[sv.size()] = '\0';
        cstr = stack_buf;
    } else {
        heap_buf.assign(sv.data(), sv.size());
        cstr = heap_buf.c_str();
    }

    const int saved_errno = errno;
    detail::ScopedCLocale locale_guard;
    errno = 0;
    char* end = nullptr;
    const double val = std::strtod(cstr, &end);
    errno = saved_errno;

    if (end != cstr + sv.size()) {
        return false;
    }
    if (!std::isfinite(val)) {
        return false;
    }
    out = val;
    return true;
}

} // namespace pipeline
} // namespace myframe
