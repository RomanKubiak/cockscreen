set(COCKSCREEN_WINDOWS_QT_ROOT "" CACHE PATH "Qt installation root built for Windows x86_64 MinGW (e.g. /opt/Qt/6.x.x/mingw_64)")

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(CMAKE_SYSTEM_VERSION 10)

set(_cockscreen_target_triplet "x86_64-w64-mingw32")

# Prefer the system-installed MinGW-w64 toolchain; override by setting
# COCKSCREEN_MINGW_TOOLCHAIN_DIR to a custom install prefix.
set(COCKSCREEN_MINGW_TOOLCHAIN_DIR "" CACHE PATH "Optional prefix containing the MinGW-w64 bin/ directory")

if(COCKSCREEN_MINGW_TOOLCHAIN_DIR)
    set(_cockscreen_compiler_prefix "${COCKSCREEN_MINGW_TOOLCHAIN_DIR}/bin/${_cockscreen_target_triplet}")
else()
    set(_cockscreen_compiler_prefix "${_cockscreen_target_triplet}")
endif()

find_program(_cockscreen_mingw_cxx NAMES "${_cockscreen_compiler_prefix}-g++" REQUIRED)
find_program(_cockscreen_mingw_cc  NAMES "${_cockscreen_compiler_prefix}-gcc"  REQUIRED)

if(NOT _cockscreen_mingw_cxx)
    message(FATAL_ERROR
        "MinGW-w64 cross compiler not found (${_cockscreen_target_triplet}-g++). "
        "Install it with: sudo apt install g++-mingw-w64-x86-64\n"
        "Or set COCKSCREEN_MINGW_TOOLCHAIN_DIR to a custom MinGW-w64 prefix.")
endif()

set(CMAKE_C_COMPILER   "${_cockscreen_mingw_cc}")
set(CMAKE_CXX_COMPILER "${_cockscreen_mingw_cxx}")
set(CMAKE_RC_COMPILER  "${_cockscreen_target_triplet}-windres")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Tell CMake where to look for Windows libraries/headers and where NOT to look
# for the host system libraries.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

if(COCKSCREEN_WINDOWS_QT_ROOT)
    list(APPEND CMAKE_PREFIX_PATH "${COCKSCREEN_WINDOWS_QT_ROOT}")
    list(APPEND CMAKE_FIND_ROOT_PATH "${COCKSCREEN_WINDOWS_QT_ROOT}")
endif()

# When cross-compiling, AUTOMOC must use the host moc binary, not the Windows
# moc.exe from the target Qt installation (which cannot run on the build host).
# Qt6's own cross-compile mechanism honours QT_HOST_PATH_CMAKE_DIR, which is
# set by the QT_HOST_PATH preset variable, but the host toolchain must also be
# explicitly discovered before the target one overwrites the cached moc path.
find_program(_cockscreen_host_moc
    NAMES moc moc-qt6
    PATHS /usr/lib/qt6/libexec /usr/bin
    NO_CMAKE_FIND_ROOT_PATH
    NO_DEFAULT_PATH
)
if(_cockscreen_host_moc)
    set(QT_MOC_EXECUTABLE "${_cockscreen_host_moc}" CACHE FILEPATH "Host moc executable for cross AUTOMOC" FORCE)
endif()
