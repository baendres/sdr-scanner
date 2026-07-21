// Regression test for protocol::soapyDeviceToJson - a real bug hit on real hardware: a
// discovered device that doesn't report a "serial" kwarg (common - not every SoapySDR driver
// module populates one) crashed the GET /api/receivers/scan request with
// "basic_string: construction from null is not valid". The cause was a ternary directly
// between a std::string and nullptr - nullptr there implicitly converts to `const char*`
// (nullptr converts to any pointer type) and then constructs a std::string from it, which is
// UB/throws at runtime. See the fix's comment on soapyDeviceToJson for the actual fix.

#include <catch2/catch_test_macros.hpp>

#include "../src/control/Protocol.h"

using namespace sdrscan;

TEST_CASE("soapyDeviceToJson handles a device with no serial kwarg") {
    std::map<std::string, std::string> kwargs = {
        {"driver", "hackrf"},
        {"label", "HackRF One"},
    };
    auto j = protocol::soapyDeviceToJson(kwargs);
    CHECK(j.at("driver") == "hackrf");
    CHECK(j.at("label") == "HackRF One");
    CHECK(j.at("serial").is_null());
    CHECK(j.at("args").at("driver") == "hackrf");
}

TEST_CASE("soapyDeviceToJson includes serial when reported") {
    std::map<std::string, std::string> kwargs = {
        {"driver", "rtlsdr"},
        {"label", "Generic RTL2832U OEM :: 00000001"},
        {"serial", "00000001"},
    };
    auto j = protocol::soapyDeviceToJson(kwargs);
    CHECK(j.at("serial") == "00000001");
}

TEST_CASE("soapyDeviceToJson handles missing driver/label too") {
    std::map<std::string, std::string> kwargs = {{"serial", "xyz"}};
    auto j = protocol::soapyDeviceToJson(kwargs);
    CHECK(j.at("driver") == "");
    CHECK(j.at("label") == "");
    CHECK(j.at("serial") == "xyz");
}
