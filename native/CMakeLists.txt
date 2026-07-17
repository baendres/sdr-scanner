cmake_minimum_required(VERSION 3.16)
project(sdrscan_native CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)
find_package(Gnuradio "3.10" REQUIRED COMPONENTS runtime analog blocks filter fft soapy audio)
pkg_check_modules(PORTAUDIO REQUIRED portaudio-2.0)
pkg_check_modules(SOAPYSDR REQUIRED SoapySDR)

set(GNURADIO_LIBS
    gnuradio::gnuradio-runtime
    gnuradio::gnuradio-analog
    gnuradio::gnuradio-blocks
    gnuradio::gnuradio-filter
    gnuradio::gnuradio-fft
    gnuradio::gnuradio-soapy
    gnuradio::gnuradio-audio
)

###
# Core library - everything except main()/tool entry points, so tests can link it too.

add_library(sdrscan_core STATIC
    src/config/Types.cpp
    src/db/Database.cpp
    src/dsp/HelperBlocks.cpp
    src/dsp/ChannelBlockBase.cpp
    src/dsp/ChannelBlockFM.cpp
    src/dsp/ChannelBlockAM.cpp
    src/dsp/ScanWindow.cpp
    src/receiver/RfRingBuffer.cpp
    src/receiver/SoapyReceiver.cpp
    src/audio/AudioMixer.cpp
    src/audio/AudioOutputFactory.cpp
    src/audio/AudioOutputLocal.cpp
    src/audio/AudioOutputUdp.cpp
    src/audio/AudioOutputWebsocket.cpp
    src/control/HttpServer.cpp
    src/scanner/Scanner.cpp
)

target_include_directories(sdrscan_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${SQLite3_INCLUDE_DIRS}
    ${PORTAUDIO_INCLUDE_DIRS}
    ${SOAPYSDR_INCLUDE_DIRS}
)

target_link_libraries(sdrscan_core PUBLIC
    ${GNURADIO_LIBS}
    SQLite::SQLite3
    nlohmann_json::nlohmann_json
    Boost::system
    Threads::Threads
    ${PORTAUDIO_LIBRARIES}
    ${SOAPYSDR_LIBRARIES}
)

target_compile_options(sdrscan_core PUBLIC ${PORTAUDIO_CFLAGS_OTHER} ${SOAPYSDR_CFLAGS_OTHER})

###
# Main app

add_executable(sdrscan src/main.cpp)
target_link_libraries(sdrscan PRIVATE sdrscan_core)

###
# YAML import tool (only this target needs yaml-cpp)

find_package(yaml-cpp QUIET)
if(yaml-cpp_FOUND)
    add_executable(sdrscan_import_yaml tools/import_yaml.cpp)
    target_link_libraries(sdrscan_import_yaml PRIVATE sdrscan_core yaml-cpp::yaml-cpp)
else()
    message(WARNING "yaml-cpp not found - skipping sdrscan_import_yaml target (apt install libyaml-cpp-dev)")
endif()

###
# Tests

enable_testing()
add_subdirectory(tests)
