#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace myframe {
namespace search {

// Space-efficient Bloom filter using MurmurHash3-x64-128 + Kirsch-Mitzenmacher
// double hashing.
//
// Common ads/search uses:
//   - Impression / click deduplication and user-seen ad filtering
//   - Cache-penetration guard in front of search / profile storage
//   - Compact blacklist / crawler URL membership pre-filtering
//   - Portable binary serialization & bitwise-OR merging across shards
class BloomFilter {
public:
    // Automatically computes optimal bit count m and hash count k for the given
    // expected item count and target false positive rate (0 < fp_rate < 1).
    BloomFilter(std::size_t expected_items, double false_positive_rate)
        : words_(), num_bits_(0), num_hashes_(0) {
        if (expected_items == 0) {
            throw std::invalid_argument("BloomFilter expected_items must be > 0");
        }
        if (!(false_positive_rate > 0.0 && false_positive_rate < 1.0) ||
            !std::isfinite(false_positive_rate)) {
            throw std::invalid_argument(
                "BloomFilter false_positive_rate must be in (0, 1)");
        }

        static const double kLn2 = 0.693147180559945309417;
        const double raw_bits =
            -static_cast<double>(expected_items) * std::log(false_positive_rate) /
            (kLn2 * kLn2);
        const double max_bits =
            static_cast<double>(std::numeric_limits<std::size_t>::max() / 2);
        if (!std::isfinite(raw_bits) || raw_bits > max_bits) {
            throw std::invalid_argument("BloomFilter bit size overflow");
        }

        std::size_t bits = static_cast<std::size_t>(std::ceil(raw_bits));
        if (bits < 64) {
            bits = 64;
        }
        // Round up to multiple of 64 bits (one uint64_t word).
        bits = ((bits + 63) / 64) * 64;

        double raw_k = (static_cast<double>(bits) /
                        static_cast<double>(expected_items)) *
                       kLn2;
        std::size_t k = static_cast<std::size_t>(std::llround(raw_k));
        if (k < 1) {
            k = 1;
        } else if (k > 32) {
            k = 32;
        }

        num_bits_ = bits;
        num_hashes_ = k;
        words_.assign(num_bits_ / 64, 0ULL);
    }

    // Explicit-size constructor for fixed memory budgets.
    static BloomFilter with_exact_size(std::size_t num_bits, std::size_t num_hashes) {
        if (num_bits == 0 || num_hashes == 0 || num_hashes > 32) {
            throw std::invalid_argument("BloomFilter invalid exact size parameters");
        }
        if (num_bits > std::numeric_limits<std::size_t>::max() - 63) {
            throw std::invalid_argument("BloomFilter bit size overflow");
        }
        BloomFilter bf;
        bf.num_bits_ = ((num_bits + 63) / 64) * 64;
        bf.num_hashes_ = num_hashes;
        bf.words_.assign(bf.num_bits_ / 64, 0ULL);
        return bf;
    }

    void add(const void* data, std::size_t len) noexcept {
        const HashPair hp = hash128(data, len);
        const std::uint64_t h1 = hp.first;
        const std::uint64_t h2 = hp.second | 1ULL; // Odd stride prevents degenerate cycles
        for (std::size_t i = 0; i < num_hashes_; ++i) {
            const std::size_t bit = static_cast<std::size_t>((h1 + i * h2) % num_bits_);
            words_[bit >> 6] |= (std::uint64_t(1) << (bit & 63));
        }
    }

    void add(const std::string& key) noexcept {
        add(key.data(), key.size());
    }

    void add_u64(std::uint64_t key) noexcept {
        add(&key, sizeof(key));
    }

    bool possibly_contains(const void* data, std::size_t len) const noexcept {
        const HashPair hp = hash128(data, len);
        const std::uint64_t h1 = hp.first;
        const std::uint64_t h2 = hp.second | 1ULL;
        for (std::size_t i = 0; i < num_hashes_; ++i) {
            const std::size_t bit = static_cast<std::size_t>((h1 + i * h2) % num_bits_);
            if ((words_[bit >> 6] & (std::uint64_t(1) << (bit & 63))) == 0) {
                return false;
            }
        }
        return true;
    }

    bool possibly_contains(const std::string& key) const noexcept {
        return possibly_contains(key.data(), key.size());
    }

    bool possibly_contains_u64(std::uint64_t key) const noexcept {
        return possibly_contains(&key, sizeof(key));
    }

    // Bitwise-OR union from another filter with identical (num_bits, num_hashes).
    // Returns false if parameters are incompatible.
    bool merge_from(const BloomFilter& other) noexcept {
        if (num_bits_ != other.num_bits_ || num_hashes_ != other.num_hashes_) {
            return false;
        }
        for (std::size_t i = 0; i < words_.size(); ++i) {
            words_[i] |= other.words_[i];
        }
        return true;
    }

    void clear() noexcept {
        std::fill(words_.begin(), words_.end(), 0ULL);
    }

    // Portable little-endian binary snapshot:
    //   [4B magic "BF01" | 8B num_bits | 4B num_hashes | words in little-endian]
    std::string serialize() const {
        std::string out;
        out.resize(16 + words_.size() * 8);
        unsigned char* p = reinterpret_cast<unsigned char*>(&out[0]);
        p[0] = 'B';
        p[1] = 'F';
        p[2] = '0';
        p[3] = '1';
        write_le64(p + 4, static_cast<std::uint64_t>(num_bits_));
        write_le32(p + 12, static_cast<std::uint32_t>(num_hashes_));
        for (std::size_t i = 0; i < words_.size(); ++i) {
            write_le64(p + 16 + i * 8, words_[i]);
        }
        return out;
    }

    static bool deserialize(const std::string& raw, BloomFilter& out) {
        if (raw.size() < 16) {
            return false;
        }
        const unsigned char* p = reinterpret_cast<const unsigned char*>(raw.data());
        if (p[0] != 'B' || p[1] != 'F' || p[2] != '0' || p[3] != '1') {
            return false;
        }
        const std::uint64_t bits64 = read_le64(p + 4);
        const std::uint32_t hashes = read_le32(p + 12);
        if (bits64 == 0 || (bits64 % 64) != 0 || hashes == 0 || hashes > 32) {
            return false;
        }
        const std::uint64_t word_count64 = bits64 / 64;
        if (word_count64 > (raw.size() - 16) / 8 ||
            raw.size() != 16 + static_cast<std::size_t>(word_count64) * 8) {
            return false;
        }

        BloomFilter tmp;
        tmp.num_bits_ = static_cast<std::size_t>(bits64);
        tmp.num_hashes_ = static_cast<std::size_t>(hashes);
        tmp.words_.resize(static_cast<std::size_t>(word_count64));
        for (std::size_t i = 0; i < tmp.words_.size(); ++i) {
            tmp.words_[i] = read_le64(p + 16 + i * 8);
        }
        out = std::move(tmp);
        return true;
    }

    std::size_t bit_count() const noexcept { return num_bits_; }
    std::size_t hash_count() const noexcept { return num_hashes_; }
    std::size_t byte_size() const noexcept { return words_.size() * sizeof(std::uint64_t); }

private:
    typedef std::pair<std::uint64_t, std::uint64_t> HashPair;

    BloomFilter() : words_(), num_bits_(0), num_hashes_(0) {}

    static std::uint64_t rotl64(std::uint64_t x, int r) noexcept {
        return (x << r) | (x >> (64 - r));
    }

    static std::uint64_t fmix64(std::uint64_t k) noexcept {
        k ^= k >> 33;
        k *= 0xff51afd7ed558ccdULL;
        k ^= k >> 33;
        k *= 0xc4ceb9fe1a85ec53ULL;
        k ^= k >> 33;
        return k;
    }

    static void write_le32(unsigned char* p, std::uint32_t v) noexcept {
        p[0] = static_cast<unsigned char>(v & 0xffU);
        p[1] = static_cast<unsigned char>((v >> 8) & 0xffU);
        p[2] = static_cast<unsigned char>((v >> 16) & 0xffU);
        p[3] = static_cast<unsigned char>((v >> 24) & 0xffU);
    }

    static void write_le64(unsigned char* p, std::uint64_t v) noexcept {
        for (int i = 0; i < 8; ++i) {
            p[i] = static_cast<unsigned char>((v >> (i * 8)) & 0xffULL);
        }
    }

    static std::uint32_t read_le32(const unsigned char* p) noexcept {
        return static_cast<std::uint32_t>(p[0]) |
               (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16) |
               (static_cast<std::uint32_t>(p[3]) << 24);
    }

    static std::uint64_t read_le64(const unsigned char* p) noexcept {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= (static_cast<std::uint64_t>(p[i]) << (i * 8));
        }
        return v;
    }

    // MurmurHash3_x64_128 (endian-independent, alignment-safe via memcpy).
    static HashPair hash128(const void* key, std::size_t len) noexcept {
        const unsigned char* data = static_cast<const unsigned char*>(key);
        const std::size_t nblocks = len / 16;

        std::uint64_t h1 = 0x9747b28c1f2e3d4cULL;
        std::uint64_t h2 = 0x5a8279996ed9eba1ULL;

        static const std::uint64_t c1 = 0x87c37b91114253d5ULL;
        static const std::uint64_t c2 = 0x4cf5ad432745937fULL;

        for (std::size_t i = 0; i < nblocks; ++i) {
            std::uint64_t k1 = read_le64(data + i * 16);
            std::uint64_t k2 = read_le64(data + i * 16 + 8);

            k1 *= c1;
            k1 = rotl64(k1, 31);
            k1 *= c2;
            h1 ^= k1;
            h1 = rotl64(h1, 27);
            h1 += h2;
            h1 = h1 * 5 + 0x52dce729ULL;

            k2 *= c2;
            k2 = rotl64(k2, 33);
            k2 *= c1;
            h2 ^= k2;
            h2 = rotl64(h2, 31);
            h2 += h1;
            h2 = h2 * 5 + 0x38495ab5ULL;
        }

        const unsigned char* tail = data + nblocks * 16;
        std::uint64_t k1 = 0;
        std::uint64_t k2 = 0;
        switch (len & 15) {
            case 15: k2 ^= static_cast<std::uint64_t>(tail[14]) << 48; /* fallthrough */
            case 14: k2 ^= static_cast<std::uint64_t>(tail[13]) << 40; /* fallthrough */
            case 13: k2 ^= static_cast<std::uint64_t>(tail[12]) << 32; /* fallthrough */
            case 12: k2 ^= static_cast<std::uint64_t>(tail[11]) << 24; /* fallthrough */
            case 11: k2 ^= static_cast<std::uint64_t>(tail[10]) << 16; /* fallthrough */
            case 10: k2 ^= static_cast<std::uint64_t>(tail[9]) << 8;   /* fallthrough */
            case 9:
                k2 ^= static_cast<std::uint64_t>(tail[8]);
                k2 *= c2;
                k2 = rotl64(k2, 33);
                k2 *= c1;
                h2 ^= k2;
                /* fallthrough */
            case 8: k1 ^= static_cast<std::uint64_t>(tail[7]) << 56; /* fallthrough */
            case 7: k1 ^= static_cast<std::uint64_t>(tail[6]) << 48; /* fallthrough */
            case 6: k1 ^= static_cast<std::uint64_t>(tail[5]) << 40; /* fallthrough */
            case 5: k1 ^= static_cast<std::uint64_t>(tail[4]) << 32; /* fallthrough */
            case 4: k1 ^= static_cast<std::uint64_t>(tail[3]) << 24; /* fallthrough */
            case 3: k1 ^= static_cast<std::uint64_t>(tail[2]) << 16; /* fallthrough */
            case 2: k1 ^= static_cast<std::uint64_t>(tail[1]) << 8;  /* fallthrough */
            case 1:
                k1 ^= static_cast<std::uint64_t>(tail[0]);
                k1 *= c1;
                k1 = rotl64(k1, 31);
                k1 *= c2;
                h1 ^= k1;
                break;
            default:
                break;
        }

        h1 ^= static_cast<std::uint64_t>(len);
        h2 ^= static_cast<std::uint64_t>(len);
        h1 += h2;
        h2 += h1;
        h1 = fmix64(h1);
        h2 = fmix64(h2);
        h1 += h2;
        h2 += h1;
        return HashPair(h1, h2);
    }

    std::vector<std::uint64_t> words_;
    std::size_t num_bits_;
    std::size_t num_hashes_;
};

} // namespace search
} // namespace myframe
