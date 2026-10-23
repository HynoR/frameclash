# frameclash-panel: SteamVR dashboard panel for FrameClash. MIT License (see LICENSE, THIRD_PARTY_NOTICES.md
# at the repository root).
cmake_minimum_required(VERSION 3.16)
project(frameclash-panel VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Where libopenvr_api.so is at link time. Nothing is vendored: on the Frame (aarch64) it is SteamVR's own copy,
# which is also used at run time (rpath below), as frameeyeosc does. Elsewhere pass -DOPENVR_LIB_DIR=... .
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(_ovr_libdir "/opt/steamvr/bin/linuxarm64")
  set(_ovr_runtime "/opt/steamvr/bin/linuxarm64:$ORIGIN")
else()
  set(_ovr_libdir "")
  set(_ovr_runtime "$ORIGIN")
endif()
set(OPENVR_LIB_DIR "${_ovr_libdir}" CACHE PATH "dir with libopenvr_api.so")
set(OPENVR_RUNTIME_RPATH "${_ovr_runtime}" CACHE STRING "rpath for libopenvr_api.so at run time")

find_package(PkgConfig REQUIRED)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(VULKAN REQUIRED IMPORTED_TARGET vulkan)
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb)
find_package(Threads REQUIRED)

find_library(OPENVR_API openvr_api PATHS "${OPENVR_LIB_DIR}" NO_DEFAULT_PATH)
if(NOT OPENVR_API)
  find_library(OPENVR_API openvr_api)
endif()
if(NOT OPENVR_API)
  message(FATAL_ERROR "libopenvr_api.so not found: set -DOPENVR_LIB_DIR=... (on the Frame: /opt/steamvr/bin/linuxarm64, "
                      "i.e. SteamVR must be installed)")
endif()

add_executable(frameclash-panel
  src/main.cpp
  src/api.cpp
  src/model.cpp
  src/ui.cpp
  src/ui_profiles.cpp
  src/spawn.cpp
  src/vr_overlay.cpp
  src/third_party_frameeyeosc/draw.cpp
  src/third_party_frameeyeosc/icons.cpp
  src/third_party_frameeyeosc/text_layout.cpp
  src/third_party_frameeyeosc/json.cpp
  src/third_party_frameeyeosc/vk_texture.cpp)
target_include_directories(frameclash-panel PRIVATE src src/third_party_frameeyeosc third_party/openvr)
target_compile_definitions(frameclash-panel PRIVATE FRAMECLASH_PANEL_VERSION="${PROJECT_VERSION}")
target_compile_options(frameclash-panel PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(frameclash-panel PRIVATE PkgConfig::CAIRO PkgConfig::FREETYPE PkgConfig::VULKAN PkgConfig::XCB
                      Threads::Threads "${OPENVR_API}")
# libstdc++/libgcc linked statically: the binary then needs only glibc, cairo, freetype, vulkan and openvr from the
# system (no dependency on the Frame's exact GCC version)
target_link_options(frameclash-panel PRIVATE -static-libstdc++ -static-libgcc)
set_target_properties(frameclash-panel PROPERTIES
  BUILD_RPATH "${OPENVR_LIB_DIR}"
  INSTALL_RPATH "${OPENVR_RUNTIME_RPATH}"
  BUILD_WITH_INSTALL_RPATH ON
  SKIP_BUILD_RPATH OFF)

add_executable(frameclash-panel-tests tests/tests.cpp src/api.cpp src/spawn.cpp src/third_party_frameeyeosc/json.cpp)
target_include_directories(frameclash-panel-tests PRIVATE src src/third_party_frameeyeosc)
target_link_libraries(frameclash-panel-tests PRIVATE PkgConfig::XCB)
enable_testing()
add_test(NAME unit COMMAND frameclash-panel-tests)
