find_package(Catch2 3 QUIET)

if(NOT Catch2_FOUND)
    message(WARNING "Catch2 not found - skipping native tests (apt install catch2)")
    return()
endif()

include(CTest)
include(Catch)

add_executable(sdrscan_tests
    test_database.cpp
    test_channel_fm.cpp
    test_channel_eas.cpp
    test_ctcss_squelch.cpp
    test_squelch_debounce.cpp
    test_scanner_config.cpp
    test_icecast_output.cpp
    test_audio_output_udp.cpp
    test_selector_hop.cpp
    test_protocol.cpp
    test_audio_mixer_watchdog.cpp
    test_channel_dmr.cpp
)
target_link_libraries(sdrscan_tests PRIVATE sdrscan_core Catch2::Catch2WithMain)

catch_discover_tests(sdrscan_tests)
