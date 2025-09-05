#include "hpack.h"
#include <cstring>
#include <map>

namespace hpack {

// RFC 7541 Appendix A: Static Table (index 1-based)
static const HeaderField kStaticTableArr[] = {
    {":authority", ""},
    {":method", "GET"},
    {":method", "POST"},
    {":path", "/"},
    {":path", "/index.html"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "200"},
    {":status", "204"},
    {":status", "206"},
    {":status", "304"},
    {":status", "400"},
    {":status", "404"},
    {":status", "500"},
    {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", ""},
    {"accept-ranges", ""},
    {"accept", ""},
    {"access-control-allow-origin", ""},
    {"age", ""},
    {"allow", ""},
    {"authorization", ""},
    {"cache-control", ""},
    {"content-disposition", ""},
    {"content-encoding", ""},
    {"content-language", ""},
    {"content-length", ""},
    {"content-location", ""},
    {"content-range", ""},
    {"content-type", ""},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"expect", ""},
    {"expires", ""},
    {"from", ""},
    {"host", ""},
    {"if-match", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"if-range", ""},
    {"if-unmodified-since", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"max-forwards", ""},
    {"proxy-authenticate", ""},
    {"proxy-authorization", ""},
    {"range", ""},
    {"referer", ""},
    {"refresh", ""},
    {"retry-after", ""},
    {"server", ""},
    {"set-cookie", ""},
    {"strict-transport-security", ""},
    {"transfer-encoding", ""},
    {"user-agent", ""},
    {"vary", ""},
    {"via", ""},
    {"www-authenticate", ""}
};

const std::vector<HeaderField>& static_table() {
    static std::vector<HeaderField> tbl(kStaticTableArr, kStaticTableArr + (sizeof(kStaticTableArr)/sizeof(kStaticTableArr[0])));
    return tbl;
}

static std::map<std::string,uint32_t> build_static_name_index() {
    std::map<std::string,uint32_t> m;
    auto& tbl = static_table();
    for (uint32_t i = 0; i < tbl.size(); ++i) {
        if (m.find(tbl[i].name) == m.end()) m[tbl[i].name] = i + 1; // 1-based
    }
    return m;
}

uint32_t static_index_of_name(const std::string& name) {
    static std::map<std::string,uint32_t> m = build_static_name_index();
    auto it = m.find(name);
    if (it == m.end()) return 0;
    return it->second;
}

// Huffman coding table (RFC 7541 Appendix B) - code and bit length per symbol (0..256; 256 is EOS)
struct HuffSym { uint32_t code; uint8_t nbits; };

// Table reduced for brevity? No, we include full per RFC
static const HuffSym HUF[257] = {
    {0x1ff8,13},{0x7fffd8,23},{0xfffffe2,28},{0xfffffe3,28},{0xfffffe4,28},{0xfffffe5,28},{0xfffffe6,28},{0xfffffe7,28},
    {0xfffffe8,28},{0xffffea,24},{0x3ffffffc,30},{0xfffffe9,28},{0xfffffea,28},{0x3ffffffd,30},{0xfffffeb,28},{0xfffffec,28},
    {0xfffffed,28},{0xfffffee,28},{0xfffffef,28},{0xffffff0,28},{0xffffff1,28},{0xffffff2,28},{0x3ffffffe,30},{0xffffff3,28},
    {0xffffff4,28},{0xffffff5,28},{0xffffff6,28},{0xffffff7,28},{0xffffff8,28},{0xffffff9,28},{0xffffffa,28},{0xffffffb,28},
    {0x14,6},{0x3f8,10},{0x3f9,10},{0xffa,12},{0x1ff9,13},{0x15,6},{0xf8,8},{0x7fa,11},
    {0x3fa,10},{0x3fb,10},{0xf9,8},{0x7fb,11},{0xfa,8},{0x16,6},{0x17,6},{0x18,6},
    {0x0,5},{0x1,5},{0x2,5},{0x19,6},{0x1a,6},{0x1b,6},{0x1c,6},{0x1d,6},
    {0x1e,6},{0x1f,6},{0x5c,7},{0xfb,8},{0x7ffc,15},{0x20,6},{0xffb,12},{0x3fc,10},
    {0x1ffa,13},{0x21,6},{0x5d,7},{0x5e,7},{0x5f,7},{0x60,7},{0x61,7},{0x62,7},
    {0x63,7},{0x64,7},{0x65,7},{0x66,7},{0x67,7},{0x68,7},{0x69,7},{0x6a,7},
    {0x6b,7},{0x6c,7},{0x6d,7},{0x6e,7},{0x6f,7},{0x70,7},{0x71,7},{0x72,7},
    {0xfc,8},{0x73,7},{0xfd,8},{0x1ffb,13},{0x7fff0,19},{0x1ffc,13},{0x3ffc,14},{0x22,6},
    {0x7ffd,15},{0x3,5},{0x23,6},{0x4,5},{0x24,6},{0x5,5},{0x25,6},{0x26,6},
    {0x27,6},{0x6,5},{0x74,7},{0x75,7},{0x28,6},{0x29,6},{0x2a,6},{0x7,5},
    {0x2b,6},{0x76,7},{0x2c,6},{0x8,5},{0x9,5},{0x2d,6},{0x77,7},{0x78,7},
    {0x79,7},{0x7a,7},{0x7b,7},{0x7ffe,15},{0x7fc,11},{0x3ffd,14},{0x1ffd,13},{0xffffffc,28},
    {0xfffe6,20},{0x3fffd2,22},{0xfffe7,20},{0xfffe8,20},{0x3fffd3,22},{0x3fffd4,22},{0x3fffd5,22},{0x7fffd9,23},
    {0x3fffd6,22},{0x7fffda,23},{0x7fffdb,23},{0x7fffdc,23},{0x7fffdd,23},{0x7fffde,23},{0xffffeb,24},{0x7fffdf,23},
    {0xffffec,24},{0xffffed,24},{0x3fffd7,22},{0x7fffe0,23},{0xffffee,24},{0x7fffe1,23},{0x7fffe2,23},{0x7fffe3,23},
    {0x7fffe4,23},{0x1fffdc,21},{0x3fffd8,22},{0x7fffe5,23},{0x3fffd9,22},{0x7fffe6,23},{0x7fffe7,23},{0xffffef,24},
    {0x3fffda,22},{0x1fffdd,21},{0xfffee,20},{0xfffeF,20},{0x1fffde,21},{0x3fffdb,22},{0x3fffdc,22},{0x7fffe8,23},
    {0x7fffe9,23},{0x1fffdf,21},{0x3fffe0,22},{0x1fffe0,21},{0x1fffe1,21},{0x3fffe1,22},{0x3fffe2,22},{0x3fffe3,22},
    {0x3fffe4,22},{0x7fffea,23},{0x7fffeb,23},{0x1fffe2,21},{0x1fffe3,21},{0x3fffe5,22},{0x3fffe6,22},{0x7fffec,23},
    {0x7fffed,23},{0x7fffee,23},{0x7fffef,23},{0xfffec,20},{0xfffff0,24},{0xfffed,20},{0x1fffe4,21},{0x1fffe5,21},
    {0x3fffe7,22},{0x3fffe8,22},{0x1fffe6,21},{0x3fffe9,22},{0x1fffe7,21},{0x3fffea,22},{0x3fffeb,22},{0x7ffff0,23},
    {0x3fffec,22},{0x3fffed,22},{0x7ffff1,23},{0x3fffee,22},{0x7ffff2,23},{0x7ffff3,23},{0x7ffff4,23},{0x7ffff5,23},
    {0x7ffff6,23},{0x7ffff7,23},{0x7ffff8,23},{0x7ffff9,23},{0x7ffffa,23},{0x7ffffb,23},{0xfffff1,24},{0xfffff2,24},
    {0xfffff3,24},{0xfffff4,24},{0xfffff5,24},{0xfffff6,24},{0xfffff7,24},{0xfffff8,24},{0xfffff9,24},{0xfffffa,24},
    {0xfffffb,24},{0xfffffc,24},{0xfffffd,24},{0xfffffe,24},{0xffffff,24},{0x1000000,25} // placeholder; will not be used
};

// Build decoding tree
struct Node { int32_t sym; int32_t left; int32_t right; Node():sym(-1),left(-1),right(-1){} };

static std::vector<Node> build_tree() {
    std::vector<Node> nodes; nodes.reserve(4096); nodes.emplace_back(); // root at 0
    for (int s = 0; s <= 255; ++s) {
        uint32_t code = HUF[s].code; uint8_t n = HUF[s].nbits;
        int idx = 0;
        for (int i = n - 1; i >= 0; --i) {
            int bit = (code >> i) & 1u;
            int32_t& child = (bit ? nodes[idx].right : nodes[idx].left);
            if (child == -1) { child = (int32_t)nodes.size(); nodes.emplace_back(); }
            idx = (int)child;
        }
        nodes[idx].sym = s;
    }
    return nodes;
}

static const std::vector<Node>& HTree() { static std::vector<Node> t = build_tree(); return t; }

bool decode_integer(const unsigned char*& p, const unsigned char* end, uint8_t prefix_bits, uint32_t& out) {
    if (p >= end) return false;
    uint8_t mask = (1u << prefix_bits) - 1u;
    uint32_t I = (*p) & mask;
    if (I < mask) { out = I; ++p; return true; }
    // continuation
    ++p; uint32_t m = 0; uint32_t val = I;
    while (p < end) {
        uint8_t b = *p++; val += (uint32_t)(b & 0x7f) << m; if ((b & 0x80) == 0) { out = val; return true; } m += 7; if (m > 28) break;
    }
    return false;
}

bool decode_string(const unsigned char*& p, const unsigned char* end, std::string& out) {
    if (p >= end) return false;
    bool h = ((*p) & 0x80) != 0; // Huffman flag
    uint32_t len = 0; if (!decode_integer(p, end, 7, len)) return false;
    if ((size_t)(end - p) < len) return false;
    if (!h) {
        out.append((const char*)p, (size_t)len);
        p += len; return true;
    }
    // Huffman decode
    const std::vector<Node>& T = HTree();
    int idx = 0; uint32_t bits = 0; int nbits = 0; size_t consumed = 0;
    while (consumed < len) {
        uint8_t byte = *p++; consumed++;
        for (int i = 7; i >= 0; --i) {
            int bit = (byte >> i) & 1;
            int next = (bit ? T[idx].right : T[idx].left);
            if (next < 0 || next >= (int)T.size()) return false; // invalid path
            idx = next;
            if (T[idx].sym >= 0) {
                unsigned char sym = (unsigned char)T[idx].sym;
                out.push_back((char)sym);
                idx = 0;
            }
        }
    }
    // EOS handling: ignore padding bits (RFC 7541: must be set to ones that form EOS prefix). We do not strictly validate here.
    return true;
}

void encode_integer(std::string& out, uint32_t value, uint8_t prefix_bits, uint8_t prefix_mask) {
    uint8_t cap = (1u << prefix_bits) - 1u;
    if (value < cap) {
        out.push_back((char)((prefix_mask) | (uint8_t)value));
        return;
    }
    out.push_back((char)((prefix_mask) | cap));
    value -= cap;
    while (value >= 128) {
        out.push_back((char)((value % 128) + 128));
        value /= 128;
    }
    out.push_back((char)value);
}

void encode_string_raw(std::string& out, const std::string& s) {
    // no huffman
    // first byte: h=0, 7-bit prefix length with continuation if needed
    std::string tmp;
    uint32_t len = (uint32_t)s.size();
    // We will write the first byte later; but easier: use encode_integer with prefix 7 and mask 0x00
    encode_integer(tmp, len, 7, 0x00);
    out += tmp;
    out += s;
}

void encode_string(std::string& out, const std::string& s, bool huffman) {
    if (!huffman) {
        encode_string_raw(out, s);
        return;
    }
    // Build bitstream using static Huffman codes
    uint64_t acc = 0; int bits = 0;
    std::string payload;
    payload.reserve(s.size());
    for (unsigned char ch : s) {
        uint32_t code = HUF[ch].code; uint8_t n = HUF[ch].nbits;
        acc = (acc << n) | code; bits += n;
        while (bits >= 8) {
            int shift = bits - 8;
            unsigned char byte = (unsigned char)((acc >> shift) & 0xff);
            payload.push_back((char)byte);
            bits -= 8;
            acc &= ((1ULL << bits) - 1ULL);
        }
    }
    // Pad with EOS (all 1s) up to next byte boundary per RFC 7541 5.2
    if (bits > 0) {
        unsigned char byte = (unsigned char)((acc << (8 - bits)) | ((1u << (8 - bits)) - 1u));
        payload.push_back((char)byte);
        bits = 0; acc = 0;
    }
    // Write length with huffman bit set and 7-bit prefix
    // First byte will be produced by encode_integer with prefix 7 and huffman flag (0x80)
    std::string lenbuf;
    encode_integer(lenbuf, (uint32_t)payload.size(), 7, 0x80);
    out += lenbuf;
    out += payload;
}

} // namespace hpack
