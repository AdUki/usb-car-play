cmake_minimum_required(VERSION 3.16)
project(btdisk CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter)
# The source the build came from, in the log's start line (the image's recipe sets it).
set(BTDISK_VERSION "dev" CACHE STRING "btdisk's version in the log")

find_package(PkgConfig REQUIRED)
# sd-bus, for BlueZ's D-Bus API (the phone's player, pairing, reconnects). The image runs
# systemd, so the library is on the board anyway. On a dev box: sudo apt install libsystemd-dev
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
# libsbc decodes the A2DP stream: the BlueZ project's SBC library, small and in poky.
pkg_check_modules(SBC REQUIRED IMPORTED_TARGET sbc)
# AAC (fdk-aac) and aptX / aptX HD (libfreeaptx) when they are there; SBC alone without them.
# BTDISK_ALL_CODECS makes them required (the image's build). On a dev box:
#   sudo apt install libfdk-aac-dev libfreeaptx-dev
option(BTDISK_ALL_CODECS "Fail without fdk-aac and libfreeaptx" OFF)
if(BTDISK_ALL_CODECS)
  set(CODEC_REQUIRED REQUIRED)
endif()
pkg_check_modules(FDKAAC ${CODEC_REQUIRED} IMPORTED_TARGET fdk-aac)
pkg_check_modules(FREEAPTX ${CODEC_REQUIRED} IMPORTED_TARGET libfreeaptx)
find_package(Threads REQUIRED)

# Everything but main(), so the tests link the same code the daemon runs.
add_library(btdisk_core STATIC
  src/a2dp.cpp
  src/bluez.cpp
  src/codecs.cpp
  src/config.cpp
  src/drift.cpp
  src/fat.cpp
  src/ffs.cpp
  src/formats.cpp
  src/livefile.cpp
  src/log.cpp
  src/player.cpp
  src/scsi.cpp
  src/stream.cpp)
target_include_directories(btdisk_core PUBLIC src)
target_compile_definitions(btdisk_core PRIVATE BTDISK_VERSION="${BTDISK_VERSION}")
target_link_libraries(btdisk_core PUBLIC PkgConfig::SYSTEMD PkgConfig::SBC Threads::Threads)
if(FDKAAC_FOUND)
  target_compile_definitions(btdisk_core PUBLIC BTDISK_AAC)
  target_link_libraries(btdisk_core PUBLIC PkgConfig::FDKAAC)
else()
  message(WARNING "fdk-aac not found: no AAC")
endif()
if(FREEAPTX_FOUND)
  target_compile_definitions(btdisk_core PUBLIC BTDISK_APTX)
  target_link_libraries(btdisk_core PUBLIC PkgConfig::FREEAPTX)
else()
  message(WARNING "libfreeaptx not found: no aptX")
endif()

add_executable(btdisk src/main.cpp)
target_link_libraries(btdisk PRIVATE btdisk_core)
install(TARGETS btdisk RUNTIME DESTINATION bin)

option(BTDISK_TESTS "Build the tests" ON)
if(BTDISK_TESTS)
  enable_testing()
  add_executable(btdisk-tests tests/test_main.cpp tests/test_drift.cpp tests/radio_sim.cpp)
  target_link_libraries(btdisk-tests PRIVATE btdisk_core)
  add_test(NAME unit COMMAND btdisk-tests --unit)
  add_test(NAME radio-sim COMMAND btdisk-tests --radio)
endif()

option(BTDISK_TOOLS "Build the test tools (radio-play, fake-phone)" ON)
if(BTDISK_TOOLS)
# A head unit's way of reading a file, for tests through a real USB host stack.
add_executable(radio-play tools/radio_play.cpp)

# A scripted phone for end-to-end tests: an A2DP SBC source playing a sine per track, and an
# AVRCP player (titles, next/previous) registered with BlueZ.
add_executable(fake-phone tools/fake_phone.cpp)
target_link_libraries(fake-phone PRIVATE PkgConfig::SYSTEMD PkgConfig::SBC)
endif()
