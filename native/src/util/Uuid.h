#pragma once

#include <cstdint>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>

namespace sdrscan {

// Minimal random (v4-ish) UUID generator - good enough as an internal id, not
// meant to be a strict RFC4122 implementation.
inline std::string makeUuid() {
    static thread_local std::random_device rd;
    static thread_local std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t a = dist(gen), b = dist(gen);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0')
        << std::setw(8) << static_cast<uint32_t>(a >> 32) << "-"
        << std::setw(4) << static_cast<uint16_t>(a >> 16) << "-"
        << "4" << std::setw(3) << static_cast<uint16_t>(a & 0x0fff) << "-"
        << std::setw(4) << static_cast<uint16_t>(((b >> 48) & 0x3fff) | 0x8000) << "-"
        << std::setw(12) << (b & 0xffffffffffffULL);
    return oss.str();
}

} // namespace sdrscan
