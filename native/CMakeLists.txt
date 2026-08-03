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

# libmp3lame ships no pkg-config file, so find it manually (apt install libmp3lame-dev).
find_path(MP3LAME_INCLUDE_DIR NAMES lame/lame.h)
find_library(MP3LAME_LIBRARY NAMES mp3lame)
if(NOT MP3LAME_INCLUDE_DIR OR NOT MP3LAME_LIBRARY)
    message(FATAL_ERROR "libmp3lame not found (needed for the Icecast audio output) - apt install libmp3lame-dev")
endif()

# DSDcc (DMR/P25p1 frame sync + AMBE/IMBE decode, wrapping mbelib) - not packaged in apt, built
# from source in native/Dockerfile / by the developer locally (see native/README.md). DSDcc's own
# CMake build generates a real libdsdcc.pc, so it's pkg-config-discoverable once installed.
pkg_check_modules(DSDCC REQUIRED libdsdcc)

# mbelib ships no pkg-config file (same situation as libmp3lame above), so find it manually.
find_path(MBE_INCLUDE_DIR NAMES mbelib.h)
find_library(MBE_LIBRARY NAMES mbe)
if(NOT MBE_INCLUDE_DIR OR NOT MBE_LIBRARY)
    message(FATAL_ERROR "mbelib not found (needed for DMR/P25 AMBE decode) - build+install f4exb/mbelib, see native/README.md")
endif()

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
    src/dsp/ChannelBlockEAS.cpp
    src/dsp/ChannelBlockDMR.cpp
    src/dsp/ChannelBlockP25Voice.cpp
    src/dsp/P25TsbkDecoder.cpp
    src/dsp/DsdccDecodeBlock.cpp
    src/dsp/ScanWindow.cpp
    src/receiver/SoapyReceiver.cpp
    src/audio/AudioMixer.cpp
    src/audio/AudioOutputFactory.cpp
    src/audio/AudioOutputLocal.cpp
    src/audio/AudioOutputUdp.cpp
    src/audio/AudioOutputWebsocket.cpp
    src/audio/AudioOutputIcecast.cpp
    src/control/HttpServer.cpp
    src/scanner/Scanner.cpp
)

target_include_directories(sdrscan_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${SQLite3_INCLUDE_DIRS}
    ${PORTAUDIO_INCLUDE_DIRS}
    ${SOAPYSDR_INCLUDE_DIRS}
    ${MP3LAME_INCLUDE_DIR}
    ${DSDCC_INCLUDE_DIRS}
    ${MBE_INCLUDE_DIR}
)

target_link_libraries(sdrscan_core PUBLIC
    ${GNURADIO_LIBS}
    SQLite::SQLite3
    nlohmann_json::nlohmann_json
    Boost::system
    Threads::Threads
    ${PORTAUDIO_LIBRARIES}
    ${SOAPYSDR_LIBRARIES}
    ${MP3LAME_LIBRARY}
    ${DSDCC_LIBRARIES}
    ${MBE_LIBRARY}
)

target_compile_options(sdrscan_core PUBLIC ${PORTAUDIO_CFLAGS_OTHER} ${SOAPYSDR_CFLAGS_OTHER} ${DSDCC_CFLAGS_OTHER})

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
