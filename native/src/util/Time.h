#pragma once

#include <chrono>

namespace sdrscan {

inline double nowUnixSeconds() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

} // namespace sdrscan
