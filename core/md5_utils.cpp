#include "md5_utils.h"

#include <openssl/evp.h>

#include <array>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <vector>

namespace myframe {

namespace {

struct MdCtxDeleter {
    void operator()(EVP_MD_CTX* ctx) const {
        if (ctx) {
            EVP_MD_CTX_free(ctx);
        }
    }
};

using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, MdCtxDeleter>;

MdCtxPtr create_md5_ctx() {
    MdCtxPtr ctx(EVP_MD_CTX_new());
    if (!ctx) {
        return nullptr;
    }

    const EVP_MD* md = EVP_md5();
    if (!md) {
        return nullptr;
    }

    if (EVP_DigestInit_ex(ctx.get(), md, nullptr) != 1) {
        return nullptr;
    }
    return ctx;
}

bool finalize(EVP_MD_CTX* ctx, unsigned char* digest, unsigned int& digest_len) {
    digest_len = 0;
    if (!digest) {
        return false;
    }
    if (EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) {
        digest_len = 0;
        return false;
    }
    return true;
}

std::string digest_to_hex(const unsigned char* digest, unsigned int digest_len) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < digest_len; ++i) {
        oss << std::setw(2) << static_cast<unsigned int>(digest[i]);
    }
    return oss.str();
}

} // namespace

bool md5_digest(const void* data, std::size_t length, unsigned char* digest, unsigned int& digest_len) {
    digest_len = 0;
    if (!digest) {
        return false;
    }

    auto ctx = create_md5_ctx();
    if (!ctx) {
        return false;
    }

    if (length > 0 && data) {
        if (EVP_DigestUpdate(ctx.get(), data, length) != 1) {
            return false;
        }
    }

    return finalize(ctx.get(), digest, digest_len);
}

bool md5_digest(const std::string& data, unsigned char* digest, unsigned int& digest_len) {
    return md5_digest(data.data(), data.size(), digest, digest_len);
}

bool md5_digest(const std::vector<std::uint8_t>& data, unsigned char* digest, unsigned int& digest_len) {
    const void* ptr = data.empty() ? nullptr : data.data();
    return md5_digest(ptr, data.size(), digest, digest_len);
}

std::string md5_hex(const void* data, std::size_t length) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (!md5_digest(data, length, digest, digest_len)) {
        return std::string();
    }
    return digest_to_hex(digest, digest_len);
}

bool md5_file_digest(const std::string& path, unsigned char* digest, unsigned int& digest_len) {
    digest_len = 0;
    if (!digest) {
        return false;
    }

    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input.is_open()) {
        return false;
    }

    auto ctx = create_md5_ctx();
    if (!ctx) {
        return false;
    }

    std::array<char, 4096> buffer;
    while (input) {
        input.read(buffer.data(), buffer.size());
        const std::streamsize read_bytes = input.gcount();
        if (read_bytes > 0) {
            if (EVP_DigestUpdate(ctx.get(), buffer.data(), static_cast<size_t>(read_bytes)) != 1) {
                return false;
            }
        }
    }

    if (!input.eof()) {
        // Stream error before EOF.
        return false;
    }

    return finalize(ctx.get(), digest, digest_len);
}

std::string md5_file_hex(const std::string& path) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (!md5_file_digest(path, digest, digest_len)) {
        return std::string();
    }
    return digest_to_hex(digest, digest_len);
}

} // namespace myframe

