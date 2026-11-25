#include "codec.h"

#include <cstdint>

namespace myframe {
namespace crypto {

namespace {

static void set_err(std::string* err, const std::string& msg) {
    if (err) *err = msg;
}

static int hex_nibble(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static std::string b64_encode_impl(const unsigned char* src, std::size_t len,
                                   const char* alpha, bool pad) {
    if (len == 0) return std::string();
    std::string out;
    out.reserve(((len + 2u) / 3u) * 4u);
    std::size_t i = 0;
    while (i + 3u <= len) {
        uint32_t v = (static_cast<uint32_t>(src[i]) << 16) |
                     (static_cast<uint32_t>(src[i + 1]) << 8) |
                     static_cast<uint32_t>(src[i + 2]);
        out.push_back(alpha[(v >> 18) & 0x3Fu]);
        out.push_back(alpha[(v >> 12) & 0x3Fu]);
        out.push_back(alpha[(v >> 6) & 0x3Fu]);
        out.push_back(alpha[v & 0x3Fu]);
        i += 3u;
    }
    std::size_t rem = len - i;
    if (rem == 1u) {
        uint32_t v = static_cast<uint32_t>(src[i]) << 16;
        out.push_back(alpha[(v >> 18) & 0x3Fu]);
        out.push_back(alpha[(v >> 12) & 0x3Fu]);
        if (pad) {
            out.push_back('=');
            out.push_back('=');
        }
    } else if (rem == 2u) {
        uint32_t v = (static_cast<uint32_t>(src[i]) << 16) |
                     (static_cast<uint32_t>(src[i + 1]) << 8);
        out.push_back(alpha[(v >> 18) & 0x3Fu]);
        out.push_back(alpha[(v >> 12) & 0x3Fu]);
        out.push_back(alpha[(v >> 6) & 0x3Fu]);
        if (pad) {
            out.push_back('=');
        }
    }
    return out;
}

static int b64_val(unsigned char c, bool url_safe) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return 26 + (c - 'a');
    if (c >= '0' && c <= '9') return 52 + (c - '0');
    if (!url_safe) {
        if (c == '+') return 62;
        if (c == '/') return 63;
    } else {
        if (c == '-') return 62;
        if (c == '_') return 63;
    }
    return -1;
}

static bool b64_decode_impl(const std::string& in, bool url_safe,
                            std::string& out, std::string* err) {
    if (in.empty()) {
        out.clear();
        return true;
    }

    const std::size_t n = in.size();
    std::size_t pad = 0;
    if (!url_safe) {
        if ((n % 4u) != 0u) {
            out.clear();
            set_err(err, "base64 length must be a multiple of 4");
            return false;
        }
        if (in[n - 1] == '=') {
            pad = 1;
            if (in[n - 2] == '=') pad = 2;
        }
    } else {
        // Accept padded (multiple of 4) or unpadded (rem 0, 2, 3).
        if (in[n - 1] == '=') {
            if ((n % 4u) != 0u) {
                out.clear();
                set_err(err, "padded base64url length must be a multiple of 4");
                return false;
            }
            pad = 1;
            if (in[n - 2] == '=') pad = 2;
        } else if ((n % 4u) == 1u) {
            out.clear();
            set_err(err, "invalid base64url length");
            return false;
        }
    }

    const std::size_t data_len = n - pad;
    std::string tmp;
    tmp.reserve((data_len * 3u) / 4u);

    std::size_t i = 0;
    while (i + 4u <= data_len) {
        int a = b64_val(static_cast<unsigned char>(in[i]), url_safe);
        int b = b64_val(static_cast<unsigned char>(in[i + 1]), url_safe);
        int c = b64_val(static_cast<unsigned char>(in[i + 2]), url_safe);
        int d = b64_val(static_cast<unsigned char>(in[i + 3]), url_safe);
        if (a < 0 || b < 0 || c < 0 || d < 0) {
            out.clear();
            set_err(err, "invalid base64 character");
            return false;
        }
        uint32_t v = (static_cast<uint32_t>(a) << 18) |
                     (static_cast<uint32_t>(b) << 12) |
                     (static_cast<uint32_t>(c) << 6) |
                     static_cast<uint32_t>(d);
        tmp.push_back(static_cast<char>((v >> 16) & 0xFFu));
        tmp.push_back(static_cast<char>((v >> 8) & 0xFFu));
        tmp.push_back(static_cast<char>(v & 0xFFu));
        i += 4u;
    }

    const std::size_t rem = data_len - i;
    if (rem == 1u) {
        out.clear();
        set_err(err, "invalid base64 trailing quantum");
        return false;
    } else if (rem == 2u) {
        if (!url_safe && pad != 2u) {
            out.clear();
            set_err(err, "invalid base64 padding");
            return false;
        }
        int a = b64_val(static_cast<unsigned char>(in[i]), url_safe);
        int b = b64_val(static_cast<unsigned char>(in[i + 1]), url_safe);
        if (a < 0 || b < 0) {
            out.clear();
            set_err(err, "invalid base64 character");
            return false;
        }
        if ((b & 0x0F) != 0) {
            out.clear();
            set_err(err, "non-zero base64 trailing bits");
            return false;
        }
        uint32_t v = (static_cast<uint32_t>(a) << 18) | (static_cast<uint32_t>(b) << 12);
        tmp.push_back(static_cast<char>((v >> 16) & 0xFFu));
    } else if (rem == 3u) {
        if (!url_safe && pad != 1u) {
            out.clear();
            set_err(err, "invalid base64 padding");
            return false;
        }
        int a = b64_val(static_cast<unsigned char>(in[i]), url_safe);
        int b = b64_val(static_cast<unsigned char>(in[i + 1]), url_safe);
        int c = b64_val(static_cast<unsigned char>(in[i + 2]), url_safe);
        if (a < 0 || b < 0 || c < 0) {
            out.clear();
            set_err(err, "invalid base64 character");
            return false;
        }
        if ((c & 0x03) != 0) {
            out.clear();
            set_err(err, "non-zero base64 trailing bits");
            return false;
        }
        uint32_t v = (static_cast<uint32_t>(a) << 18) |
                     (static_cast<uint32_t>(b) << 12) |
                     (static_cast<uint32_t>(c) << 6);
        tmp.push_back(static_cast<char>((v >> 16) & 0xFFu));
        tmp.push_back(static_cast<char>((v >> 8) & 0xFFu));
    } else if (pad != 0u) {
        out.clear();
        set_err(err, "unexpected base64 padding");
        return false;
    }

    out.swap(tmp);
    return true;
}

} // namespace

std::string hex_encode(const void* data, std::size_t len, bool uppercase) {
    if (!data || len == 0) return std::string();
    static const char kLower[] = "0123456789abcdef";
    static const char kUpper[] = "0123456789ABCDEF";
    const char* digits = uppercase ? kUpper : kLower;
    const unsigned char* p = static_cast<const unsigned char*>(data);
    std::string out;
    out.resize(len * 2u);
    for (std::size_t i = 0; i < len; ++i) {
        out[i * 2u]      = digits[(p[i] >> 4) & 0x0Fu];
        out[i * 2u + 1u] = digits[p[i] & 0x0Fu];
    }
    return out;
}

bool hex_decode(const std::string& hex, std::string& out, std::string* err) {
    if ((hex.size() % 2u) != 0u) {
        out.clear();
        set_err(err, "hex string length must be even");
        return false;
    }
    std::string tmp;
    tmp.resize(hex.size() / 2u);
    for (std::size_t i = 0; i < tmp.size(); ++i) {
        int hi = hex_nibble(static_cast<unsigned char>(hex[i * 2u]));
        int lo = hex_nibble(static_cast<unsigned char>(hex[i * 2u + 1u]));
        if (hi < 0 || lo < 0) {
            out.clear();
            set_err(err, "invalid hex character");
            return false;
        }
        tmp[i] = static_cast<char>((hi << 4) | lo);
    }
    out.swap(tmp);
    return true;
}

std::string base64_encode(const void* data, std::size_t len) {
    static const char kStd[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    return b64_encode_impl(static_cast<const unsigned char*>(data), len, kStd, true);
}

bool base64_decode(const std::string& b64, std::string& out, std::string* err) {
    return b64_decode_impl(b64, false, out, err);
}

std::string base64url_encode(const void* data, std::size_t len, bool with_padding) {
    static const char kUrl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    return b64_encode_impl(static_cast<const unsigned char*>(data), len, kUrl, with_padding);
}

bool base64url_decode(const std::string& b64url, std::string& out, std::string* err) {
    return b64_decode_impl(b64url, true, out, err);
}

} // namespace crypto
} // namespace myframe
