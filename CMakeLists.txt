cmake_minimum_required(VERSION 3.24)

project(cockscreen VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(FetchContent)

set(COCKSCREEN_PI_SYNC_HOST "atom@192.168.41.181" CACHE STRING "SSH destination for automatic Pi binary sync")
set(COCKSCREEN_PI_SYNC_ROOT "/home/atom/cockscreen" CACHE STRING "Remote Pi workspace root for automatic binary sync")

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

if(NOT WIN32)
    find_library(EGL_LIBRARY NAMES EGL REQUIRED)
    find_library(ASOUND_LIBRARY NAMES asound REQUIRED)

    add_library(EGL::EGL UNKNOWN IMPORTED)
    set_target_properties(EGL::EGL PROPERTIES IMPORTED_LOCATION "${EGL_LIBRARY}")

    add_library(ASOUND::ASOUND UNKNOWN IMPORTED)
    set_target_properties(ASOUND::ASOUND PROPERTIES IMPORTED_LOCATION "${ASOUND_LIBRARY}")
endif()

add_library(cockscreen_core)
add_library(cockscreen::core ALIAS cockscreen_core)

target_sources(cockscreen_core
    PRIVATE
        src/app/Arguments.cpp
        src/app/Config.cpp
        src/app/DeviceDiscovery.cpp
        src/runtime/audioanalysis/Processing.cpp
        src/runtime/audioanalysis/UI.cpp
        src/core/ModulationBus.cpp
        src/runtime/Application.cpp
        src/runtime/application/Support.cpp
        src/runtime/helpers/Media.cpp
        src/runtime/helpers/Shader.cpp
        src/runtime/helpers/System.cpp
        src/runtime/osc/Monitor.cpp
        src/runtime/Scene.cpp
        src/runtime/scene/Parse.cpp
        src/runtime/shadervideo/Assets.cpp
        src/runtime/shadervideo/Lifecycle.cpp
        src/runtime/shadervideo/Rendering.cpp
        src/runtime/shadervideo/Support.cpp
        src/runtime/StatusOverlay.cpp
        src/runtime/VideoWindow.cpp
)

target_sources(cockscreen_core
    PRIVATE
        src/runtime/midi/Monitor.cpp
        src/runtime/midi/PortDiscovery.cpp
)

if(NOT WIN32)
    target_sources(cockscreen_core
        PRIVATE
            src/runtime/directvideo/Lifecycle.cpp
            src/runtime/directvideo/Rendering.cpp
            src/runtime/directvideo/Support.cpp
            src/runtime/directvideo/Upload.cpp
            src/runtime/v4l2/Buffers.cpp
            src/runtime/v4l2/Capture.cpp
            src/runtime/v4l2/Formats.cpp
    )
else()
    target_sources(cockscreen_core
        PRIVATE
            src/runtime/audioanalysis/WasapiLoopback.cpp
    )
endif()

set_source_files_properties(
    src/runtime/Application.cpp
    src/runtime/directvideo/Lifecycle.cpp
    src/runtime/directvideo/Rendering.cpp
    src/runtime/shadervideo/Lifecycle.cpp
    src/runtime/shadervideo/Rendering.cpp
    PROPERTIES
    COMPILE_OPTIONS "$<$<CONFIG:Debug>:-g1>"
)

    set_source_files_properties(
        src/runtime/directvideo/Lifecycle.cpp
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
    PFFFT::PFFFT
)

if(NOT WIN32)
    target_link_libraries(cockscreen_core PUBLIC ASOUND::ASOUND EGL::EGL)
else()
    target_link_libraries(cockscreen_core PUBLIC winmm ws2_32 mmdevapi ole32 uuid psapi shell32)
endif()

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

if(CMAKE_CROSSCOMPILING AND COCKSCREEN_TARGET_PLATFORM STREQUAL "raspberry-pi-zero-2w")
    add_custom_target(cockscreen_pi_sync ALL
        DEPENDS cockscreen
        COMMAND
            ${CMAKE_COMMAND}
            -DCOCKSCREEN_PI_SYNC_HOST=${COCKSCREEN_PI_SYNC_HOST}
            -DCOCKSCREEN_PI_SYNC_ROOT=${COCKSCREEN_PI_SYNC_ROOT}
            -DCOCKSCREEN_PI_SYNC_BINARY=$<TARGET_FILE:cockscreen>
            -DCOCKSCREEN_PI_SYNC_BUILD_DIR=${CMAKE_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/sync-binary-to-pi.cmake
        VERBATIM
    )
endif()

install(TARGETS cockscreen RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    find_program(WINE_EXECUTABLE NAMES wine wine64 REQUIRED
        DOC "Wine host binary used to run windeployqt.exe during cross-compilation")

    set(_cockscreen_windeployqt "${COCKSCREEN_WINDOWS_QT_ROOT}/bin/windeployqt.exe")

    # Resolve MinGW runtime DLLs via the compiler's own search path.
    set(_cockscreen_mingw_runtime_dlls "")
    foreach(_dll IN ITEMS libgcc_s_seh-1.dll libstdc++-6.dll)
        execute_process(
            COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=${_dll}
            OUTPUT_VARIABLE _dll_path
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(_dll_path AND NOT "${_dll_path}" STREQUAL "${_dll}")
            list(APPEND _cockscreen_mingw_runtime_dlls "${_dll_path}")
        endif()
    endforeach()

    # libwinpthread-1.dll lives in the MinGW sysroot, not in GCC's lib directory.
    find_file(_cockscreen_libpthread NAMES libwinpthread-1.dll
        PATHS "/usr/x86_64-w64-mingw32/lib"
        NO_DEFAULT_PATH
    )
    if(_cockscreen_libpthread)
        list(APPEND _cockscreen_mingw_runtime_dlls "${_cockscreen_libpthread}")
    endif()

    set(_cockscreen_deploy_dir "${CMAKE_BINARY_DIR}/deploy")

    # Generate a cockscreen.ini with paths relative to the deploy folder root.
    # (scene_file and shader_directory sit beside the exe, not one level up)
    set(_cockscreen_deploy_ini "${CMAKE_BINARY_DIR}/cockscreen-deploy.ini")
    file(WRITE "${_cockscreen_deploy_ini}"
        "; Cockscreen defaults for Windows 10 64-bit\n"
        "[runtime]\n"
        "render_path=qt-shader\n"
        "window_title=cockscreen\n"
        "width=1024\n"
        "height=600\n"
        "frame_rate=30\n"
        "scene_file=scenes/windows.scene.json\n"
        "shader_directory=shaders\n"
    )

    # Include in ALL only for Release so it doesn't slow down debug iterations.
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set(_cockscreen_package_in_all ALL)
    else()
        set(_cockscreen_package_in_all "")
    endif()

    add_custom_target(cockscreen_package ${_cockscreen_package_in_all}
        DEPENDS cockscreen
        # Create a clean staging directory.
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_cockscreen_deploy_dir}"
        # Stage the executable so windeployqt deploys alongside it.
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:cockscreen>" "${_cockscreen_deploy_dir}/"
        # Run windeployqt under Wine. WINEDEBUG=-all silences Wine noise.
        # windeployqt deploys Qt DLLs and platform/multimedia/imageformat plugins
        # into the same directory as the exe it is pointed at.
        COMMAND ${CMAKE_COMMAND} -E env
                    "WINEPREFIX=${CMAKE_BINARY_DIR}/.wineprefix"
                    "WINEDEBUG=-all"
                "${WINE_EXECUTABLE}" "${_cockscreen_windeployqt}"
                    "$<IF:$<CONFIG:Release>,--release,--debug>"
                    --no-translations
                    --no-system-d3d-compiler
                    --no-opengl-sw
                    "${_cockscreen_deploy_dir}/cockscreen.exe"
        # Copy MinGW runtime DLLs that windeployqt doesn't know about.
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${_cockscreen_mingw_runtime_dlls}
                "${_cockscreen_deploy_dir}/"
        # Copy cockscreen.ini next to the exe so double-clicking works out of the box.
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_cockscreen_deploy_ini}"
                "${_cockscreen_deploy_dir}/cockscreen.ini"
        # Copy runtime asset directories.
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/shaders"   "${_cockscreen_deploy_dir}/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/scenes"    "${_cockscreen_deploy_dir}/scenes"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/resources" "${_cockscreen_deploy_dir}/resources"
        COMMENT "Packaging Windows deploy -> ${_cockscreen_deploy_dir}"
        VERBATIM
    )
endif()
