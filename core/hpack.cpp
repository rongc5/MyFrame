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

// Full canonical RFC 7541 Appendix B Huffman coding table (257 entries: 0..255 + 256 EOS).
// Verified prefix-free, Kraft sum == 1 (exact 2^30), and 256/256 matched against libnghttp2.
static const HuffSym HUF[257] = {
    {0x1ff8u,13},{0x7fffd8u,23},{0xfffffe2u,28},{0xfffffe3u,28},{0xfffffe4u,28},{0xfffffe5u,28},{0xfffffe6u,28},{0xfffffe7u,28},
    {0xfffffe8u,28},{0xffffeau,24},{0x3ffffffcu,30},{0xfffffe9u,28},{0xfffffeau,28},{0x3ffffffdu,30},{0xfffffebu,28},{0xfffffecu,28},
    {0xfffffedu,28},{0xfffffeeu,28},{0xfffffefu,28},{0xffffff0u,28},{0xffffff1u,28},{0xffffff2u,28},{0x3ffffffeu,30},{0xffffff3u,28},
    {0xffffff4u,28},{0xffffff5u,28},{0xffffff6u,28},{0xffffff7u,28},{0xffffff8u,28},{0xffffff9u,28},{0xffffffau,28},{0xffffffbu,28},
    {0x14u,6},{0x3f8u,10},{0x3f9u,10},{0xffau,12},{0x1ff9u,13},{0x15u,6},{0xf8u,8},{0x7fau,11},
    {0x3fau,10},{0x3fbu,10},{0xf9u,8},{0x7fbu,11},{0xfau,8},{0x16u,6},{0x17u,6},{0x18u,6},
    {0x0u,5},{0x1u,5},{0x2u,5},{0x19u,6},{0x1au,6},{0x1bu,6},{0x1cu,6},{0x1du,6},
    {0x1eu,6},{0x1fu,6},{0x5cu,7},{0xfbu,8},{0x7ffcu,15},{0x20u,6},{0xffbu,12},{0x3fcu,10},
    {0x1ffau,13},{0x21u,6},{0x5du,7},{0x5eu,7},{0x5fu,7},{0x60u,7},{0x61u,7},{0x62u,7},
    {0x63u,7},{0x64u,7},{0x65u,7},{0x66u,7},{0x67u,7},{0x68u,7},{0x69u,7},{0x6au,7},
    {0x6bu,7},{0x6cu,7},{0x6du,7},{0x6eu,7},{0x6fu,7},{0x70u,7},{0x71u,7},{0x72u,7},
    {0xfcu,8},{0x73u,7},{0xfdu,8},{0x1ffbu,13},{0x7fff0u,19},{0x1ffcu,13},{0x3ffcu,14},{0x22u,6},
    {0x7ffdu,15},{0x3u,5},{0x23u,6},{0x4u,5},{0x24u,6},{0x5u,5},{0x25u,6},{0x26u,6},
    {0x27u,6},{0x6u,5},{0x74u,7},{0x75u,7},{0x28u,6},{0x29u,6},{0x2au,6},{0x7u,5},
    {0x2bu,6},{0x76u,7},{0x2cu,6},{0x8u,5},{0x9u,5},{0x2du,6},{0x77u,7},{0x78u,7},
    {0x79u,7},{0x7au,7},{0x7bu,7},{0x7ffeu,15},{0x7fcu,11},{0x3ffdu,14},{0x1ffdu,13},{0xffffffcu,28},
    {0xfffe6u,20},{0x3fffd2u,22},{0xfffe7u,20},{0xfffe8u,20},{0x3fffd3u,22},{0x3fffd4u,22},{0x3fffd5u,22},{0x7fffd9u,23},
    {0x3fffd6u,22},{0x7fffdau,23},{0x7fffdbu,23},{0x7fffdcu,23},{0x7fffddu,23},{0x7fffdeu,23},{0xffffebu,24},{0x7fffdfu,23},
    {0xffffecu,24},{0xffffedu,24},{0x3fffd7u,22},{0x7fffe0u,23},{0xffffeeu,24},{0x7fffe1u,23},{0x7fffe2u,23},{0x7fffe3u,23},
    {0x7fffe4u,23},{0x1fffdcu,21},{0x3fffd8u,22},{0x7fffe5u,23},{0x3fffd9u,22},{0x7fffe6u,23},{0x7fffe7u,23},{0xffffefu,24},
    {0x3fffdau,22},{0x1fffddu,21},{0xfffe9u,20},{0x3fffdbu,22},{0x3fffdcu,22},{0x7fffe8u,23},{0x7fffe9u,23},{0x1fffdeu,21},
    {0x7fffeau,23},{0x3fffddu,22},{0x3fffdeu,22},{0xfffff0u,24},{0x1fffdfu,21},{0x3fffdfu,22},{0x7fffebu,23},{0x7fffecu,23},
    {0x1fffe0u,21},{0x1fffe1u,21},{0x3fffe0u,22},{0x1fffe2u,21},{0x7fffedu,23},{0x3fffe1u,22},{0x7fffeeu,23},{0x7fffefu,23},
    {0xfffeau,20},{0x3fffe2u,22},{0x3fffe3u,22},{0x3fffe4u,22},{0x7ffff0u,23},{0x3fffe5u,22},{0x3fffe6u,22},{0x7ffff1u,23},
    {0x3ffffe0u,26},{0x3ffffe1u,26},{0xfffebu,20},{0x7fff1u,19},{0x3fffe7u,22},{0x7ffff2u,23},{0x3fffe8u,22},{0x1ffffecu,25},
    {0x3ffffe2u,26},{0x3ffffe3u,26},{0x3ffffe4u,26},{0x7ffffdeu,27},{0x7ffffdfu,27},{0x3ffffe5u,26},{0xfffff1u,24},{0x1ffffedu,25},
    {0x7fff2u,19},{0x1fffe3u,21},{0x3ffffe6u,26},{0x7ffffe0u,27},{0x7ffffe1u,27},{0x3ffffe7u,26},{0x7ffffe2u,27},{0xfffff2u,24},
    {0x1fffe4u,21},{0x1fffe5u,21},{0x3ffffe8u,26},{0x3ffffe9u,26},{0xffffffdu,28},{0x7ffffe3u,27},{0x7ffffe4u,27},{0x7ffffe5u,27},
    {0xfffecu,20},{0xfffff3u,24},{0xfffedu,20},{0x1fffe6u,21},{0x3fffe9u,22},{0x1fffe7u,21},{0x1fffe8u,21},{0x7ffff3u,23},
    {0x3fffeau,22},{0x3fffebu,22},{0x1ffffeeu,25},{0x1ffffefu,25},{0xfffff4u,24},{0xfffff5u,24},{0x3ffffeau,26},{0x7ffff4u,23},
    {0x3ffffebu,26},{0x7ffffe6u,27},{0x3ffffecu,26},{0x3ffffedu,26},{0x7ffffe7u,27},{0x7ffffe8u,27},{0x7ffffe9u,27},{0x7ffffeau,27},
    {0x7ffffebu,27},{0xffffffeu,28},{0x7ffffecu,27},{0x7ffffedu,27},{0x7ffffeeu,27},{0x7ffffefu,27},{0x7fffff0u,27},{0x3ffffeeu,26},
    {0x3fffffffu,30}
};

// Build decoding tree (includes EOS=256 so EOS-in-stream is explicitly caught).
struct Node { int32_t sym; int32_t left; int32_t right; Node():sym(-1),left(-1),right(-1){} };

static std::vector<Node> build_tree() {
    std::vector<Node> nodes; nodes.reserve(1024); nodes.emplace_back(); // root at 0
    for (int s = 0; s <= 256; ++s) {
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
    if (p >= end || prefix_bits == 0 || prefix_bits > 8) return false;
    uint32_t mask = (1u << prefix_bits) - 1u;
    uint32_t I = (*p) & mask;
    if (I < mask) { out = I; ++p; return true; }
    // continuation (guard against shift and uint32_t overflow per RFC 7541 5.1)
    ++p;
    uint32_t m = 0;
    uint64_t val = I;
    while (p < end) {
        uint8_t b = *p++;
        if (m >= 28 && (b & 0x7fu) > 0x0fu) return false;
        uint64_t add = static_cast<uint64_t>(b & 0x7fu) << m;
        if (add > static_cast<uint64_t>(UINT32_MAX) - val) return false;
        val += add;
        if ((b & 0x80u) == 0) { out = static_cast<uint32_t>(val); return true; }
        m += 7;
        if (m > 28) return false;
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
    // RFC 7541 5.2 strict Huffman decode: reject EOS symbol inside stream,
    // padding longer than 7 bits, or any 0-bit in trailing EOS padding.
    const std::vector<Node>& T = HTree();
    int idx = 0;
    int pending_bits = 0;
    bool pending_all_ones = true;
    size_t consumed = 0;
    while (consumed < len) {
        uint8_t byte = *p++; consumed++;
        for (int i = 7; i >= 0; --i) {
            int bit = (byte >> i) & 1;
            ++pending_bits;
            if (bit == 0) pending_all_ones = false;
            int next = (bit ? T[idx].right : T[idx].left);
            if (next < 0 || next >= (int)T.size()) return false;
            idx = next;
            if (T[idx].sym >= 0) {
                if (T[idx].sym == 256) return false; // EOS must never appear in stream
                out.push_back((char)(unsigned char)T[idx].sym);
                idx = 0;
                pending_bits = 0;
                pending_all_ones = true;
            }
        }
    }
    if (pending_bits > 7 || !pending_all_ones) return false;
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
