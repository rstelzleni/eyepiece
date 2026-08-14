cmake_minimum_required(VERSION 3.24)
project(eyepiece VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(EYEPIECE_USE_OIIO "Decode with OpenImageIO when available" ON)

add_library(eyepiece_warnings INTERFACE)
target_compile_options(eyepiece_warnings INTERFACE -Wall -Wextra -Wpedantic)

find_package(SDL3 CONFIG REQUIRED)
find_package(OpenColorIO CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(EPOXY REQUIRED IMPORTED_TARGET epoxy)

# ---- image decoding backend -------------------------------------------------
# OIIO is the target backend: it brings the format coverage and the metadata
# story. It is optional only so the tree builds and runs before you install it.
set(EYEPIECE_OIIO_ACTIVE OFF)
if(EYEPIECE_USE_OIIO)
  find_package(OpenImageIO CONFIG QUIET)
  if(OpenImageIO_FOUND)
    set(EYEPIECE_OIIO_ACTIVE ON)
  endif()
endif()

if(NOT EYEPIECE_OIIO_ACTIVE)
  find_package(OpenEXR CONFIG REQUIRED)
endif()

# ---- Dear ImGui -------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.91.9b
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC SDL3::SDL3)

# ---- core library -----------------------------------------------------------
# This is the future libeyepiece. It knows nothing about ImGui, SDL, or argv.
# Keeping that true from the first commit is what makes the embeddable-library
# goal a packaging problem later instead of a rewrite.
add_library(eyepiece_core STATIC
  src/core/loader.cpp
  src/core/viewport.cpp
  src/core/color.cpp
  src/gpu/renderer.cpp
)
target_include_directories(eyepiece_core PUBLIC src third_party)
target_link_libraries(eyepiece_core
  PUBLIC OpenColorIO::OpenColorIO PkgConfig::EPOXY
  PRIVATE eyepiece_warnings
)

if(EYEPIECE_OIIO_ACTIVE)
  target_compile_definitions(eyepiece_core PUBLIC EYEPIECE_WITH_OIIO)
  target_link_libraries(eyepiece_core PUBLIC OpenImageIO::OpenImageIO)
else()
  target_link_libraries(eyepiece_core PUBLIC OpenEXR::OpenEXR)
endif()

# ---- application ------------------------------------------------------------
add_executable(eyepiece
  src/app/main.cpp
  src/app/ui.cpp
)
target_link_libraries(eyepiece PRIVATE eyepiece_core imgui SDL3::SDL3
                                       eyepiece_warnings)

install(TARGETS eyepiece RUNTIME DESTINATION bin)

message(STATUS "eyepiece: decode backend = ${EYEPIECE_OIIO_ACTIVE}")
