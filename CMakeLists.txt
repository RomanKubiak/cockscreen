cmake_minimum_required(VERSION 3.24)

project(cockscreen VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(FetchContent)

option(COCKSCREEN_ENABLE_STRICT_WARNINGS "Enable extra warning flags" ON)
set(COCKSCREEN_TARGET_PLATFORM "raspberry-pi-zero-2w" CACHE STRING "Target platform label")

set(PFFFT_USE_TYPE_DOUBLE OFF CACHE BOOL "" FORCE)
set(PFFFT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PFFFT_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(PFFFT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64")
    set(PFFFT_USE_SIMD_NEON ON CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(pffft
    GIT_REPOSITORY https://github.com/marton78/pffft.git
    GIT_TAG v1.1.0
)
FetchContent_MakeAvailable(pffft)

find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia OpenGLWidgets OpenGL)
find_library(EGL_LIBRARY NAMES EGL REQUIRED)
find_library(ASOUND_LIBRARY NAMES asound REQUIRED)

add_library(EGL::EGL UNKNOWN IMPORTED)
set_target_properties(EGL::EGL PROPERTIES IMPORTED_LOCATION "${EGL_LIBRARY}")

add_library(ASOUND::ASOUND UNKNOWN IMPORTED)
set_target_properties(ASOUND::ASOUND PROPERTIES IMPORTED_LOCATION "${ASOUND_LIBRARY}")

add_library(cockscreen_core)
add_library(cockscreen::core ALIAS cockscreen_core)

target_sources(cockscreen_core
    PRIVATE
        src/runtime/AudioAnalysisWindow.cpp
        src/core/ModulationBus.cpp
        src/runtime/Application.cpp
        src/runtime/DirectVideoWindow.cpp
        src/runtime/MidiInputMonitor.cpp
        src/runtime/RuntimeHelpers.cpp
        src/runtime/Scene.cpp
        src/runtime/ShaderVideoWindow.cpp
        src/runtime/VideoWindow.cpp
        src/runtime/V4l2Capture.cpp
)

set_source_files_properties(
    src/runtime/Application.cpp
    src/runtime/ShaderVideoWindow.cpp
    PROPERTIES
    COMPILE_OPTIONS "$<$<CONFIG:Debug>:-g1>"
)

    set_source_files_properties(
        src/runtime/DirectVideoWindow.cpp
        PROPERTIES
        COMPILE_OPTIONS "$<$<CONFIG:Release>:-fno-lto>"
    )

target_include_directories(cockscreen_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(cockscreen_core PUBLIC cxx_std_20)

target_link_libraries(cockscreen_core
    PUBLIC
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::OpenGLWidgets
    Qt6::OpenGL
    ASOUND::ASOUND
    PFFFT::PFFFT
    EGL::EGL
)

target_compile_definitions(cockscreen_core
    PUBLIC
        COCKSCREEN_TARGET_PLATFORM="${COCKSCREEN_TARGET_PLATFORM}"
)

if(COCKSCREEN_ENABLE_STRICT_WARNINGS)
    target_compile_options(cockscreen_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(cockscreen src/main.cpp)
target_link_libraries(cockscreen PRIVATE cockscreen::core)

target_compile_features(cockscreen PRIVATE cxx_std_20)

if(COCKSCREEN_ENABLE_STRICT_WARNINGS)
    target_compile_options(cockscreen PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS cockscreen RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
