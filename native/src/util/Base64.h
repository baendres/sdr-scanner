#pragma once

#include <cstdint>
#include <string>

namespace sdrscan {

// Standard RFC 4648 base64 encoding (with padding). Not reusing
// boost::beast::detail::base64 - that's an internal implementation detail of Beast, not a
// stable public API, and this is small/simple enough to just own directly.
inline std::string base64Encode(const std::string& in) {
    static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= in.size()) {
        uint32_t n = (static_cast<uint8_t>(in[i]) << 16) | (static_cast<uint8_t>(in[i + 1]) << 8) | static_cast<uint8_t>(in[i + 2]);
        out += kAlphabet[(n >> 18) & 0x3F];
        out += kAlphabet[(n >> 12) & 0x3F];
        out += kAlphabet[(n >> 6) & 0x3F];
        out += kAlphabet[n & 0x3F];
        i += 3;
    }
    size_t rem = in.size() - i;
    if (rem == 1) {
        uint32_t n = static_cast<uint8_t>(in[i]) << 16;
        out += kAlphabet[(n >> 18) & 0x3F];
        out += kAlphabet[(n >> 12) & 0x3F];
        out += "==";
    } else if (rem == 2) {
        uint32_t n = (static_cast<uint8_t>(in[i]) << 16) | (static_cast<uint8_t>(in[i + 1]) << 8);
        out += kAlphabet[(n >> 18) & 0x3F];
        out += kAlphabet[(n >> 12) & 0x3F];
        out += kAlphabet[(n >> 6) & 0x3F];
        out += "=";
    }
    return out;
}

} // namespace sdrscan
