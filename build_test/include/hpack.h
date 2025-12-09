#pragma once

#include <string>
#include <vector>
#include <utility>
#include <cstdint>

namespace hpack
{

// HPACK static table (RFC 7541 Appendix A) 1-based index
struct HeaderField { const char* name; const char* value; };
const std::vector<HeaderField>& static_table();

// Integer decoding with prefix bits (RFC 7541 5.1)
bool decode_integer(const unsigned char*& p, const unsigned char* end, uint8_t prefix_bits, uint32_t& out);

// Decode an HPACK string (length + optional huffman), append to out
bool decode_string(const unsigned char*& p, const unsigned char* end, std::string& out);

// Encode an HPACK string; if huffman=true use HPACK static Huffman table
void encode_string(std::string& out, const std::string& s, bool huffman);

// Minimal encoder helpers (no huffman, no indexing)
void encode_integer(std::string& out, uint32_t value, uint8_t prefix_bits, uint8_t prefix_mask);
void encode_string_raw(std::string& out, const std::string& s);

// Optional: lookup static index for name (returns 0 if not found)
uint32_t static_index_of_name(const std::string& name);

} // namespace hpack
