set(COCKSCREEN_PI_ZERO2W_ROOT "/work/pizero" CACHE PATH "Cross-compilation root directory for the Pi Zero 2 W toolchain and sysroot")

set(_cockscreen_toolchain_dir "${COCKSCREEN_PI_ZERO2W_ROOT}/toolchains/current")
set(_cockscreen_sysroot_dir "${COCKSCREEN_PI_ZERO2W_ROOT}/sysroot")
set(_cockscreen_target_triplet "aarch64-linux-gnu")
set(_cockscreen_compiler_triplet "aarch64-none-linux-gnu")
set(_cockscreen_multiarch_include_dir "${_cockscreen_sysroot_dir}/usr/include/${_cockscreen_target_triplet}")
set(_cockscreen_multiarch_library_dir "${_cockscreen_sysroot_dir}/usr/lib/${_cockscreen_target_triplet}")
set(_cockscreen_multiarch_system_library_dir "${_cockscreen_sysroot_dir}/lib/${_cockscreen_target_triplet}")
set(_cockscreen_startfile_prefix_flags
    "-B${_cockscreen_multiarch_library_dir} -B${_cockscreen_multiarch_system_library_dir}")
set(_cockscreen_rpath_link_flags
    "-Wl,-rpath-link,${_cockscreen_multiarch_library_dir} -Wl,-rpath-link,${_cockscreen_multiarch_system_library_dir} -Wl,-rpath-link,${_cockscreen_sysroot_dir}/usr/lib -Wl,-rpath-link,${_cockscreen_sysroot_dir}/lib")

if(NOT EXISTS "${_cockscreen_toolchain_dir}/bin/${_cockscreen_compiler_triplet}-g++")
    message(FATAL_ERROR
        "Pi Zero 2 W cross compiler not found under ${_cockscreen_toolchain_dir}. "
        "Run scripts/bootstrap-pizero-cross.sh first to populate /work/pizero.")
endif()

if(NOT EXISTS "${_cockscreen_sysroot_dir}/usr/lib/${_cockscreen_target_triplet}")
    message(FATAL_ERROR
        "Pi Zero 2 W sysroot not found under ${_cockscreen_sysroot_dir}. "
        "Run scripts/bootstrap-pizero-cross.sh first to populate /work/pizero.")
endif()

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSROOT "${_cockscreen_sysroot_dir}")
set(CMAKE_STAGING_PREFIX "${COCKSCREEN_PI_ZERO2W_ROOT}/stage")
set(CMAKE_LIBRARY_ARCHITECTURE "${_cockscreen_target_triplet}")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

find_program(_cockscreen_host_ninja NAMES ninja REQUIRED NO_CMAKE_FIND_ROOT_PATH)
set(CMAKE_MAKE_PROGRAM "${_cockscreen_host_ninja}" CACHE FILEPATH "Host Ninja executable" FORCE)

set(CMAKE_C_COMPILER "${_cockscreen_toolchain_dir}/bin/${_cockscreen_compiler_triplet}-gcc")
set(CMAKE_CXX_COMPILER "${_cockscreen_toolchain_dir}/bin/${_cockscreen_compiler_triplet}-g++")
set(CMAKE_ASM_COMPILER "${_cockscreen_toolchain_dir}/bin/${_cockscreen_compiler_triplet}-gcc")
set(CMAKE_AR "${_cockscreen_toolchain_dir}/bin/${_cockscreen_compiler_triplet}-ar")
set(CMAKE_RANLIB "${_cockscreen_toolchain_dir}/bin/${_cockscreen_compiler_triplet}-ranlib")
set(CMAKE_STRIP "${_cockscreen_toolchain_dir}/bin/${_cockscreen_compiler_triplet}-strip")

set(_cockscreen_cpu_flags "-mcpu=cortex-a53")
set(CMAKE_C_FLAGS_INIT
    "${_cockscreen_cpu_flags} --sysroot=${CMAKE_SYSROOT} ${_cockscreen_startfile_prefix_flags} -I${_cockscreen_multiarch_include_dir}")
set(CMAKE_CXX_FLAGS_INIT
    "${_cockscreen_cpu_flags} --sysroot=${CMAKE_SYSROOT} ${_cockscreen_startfile_prefix_flags} -I${_cockscreen_multiarch_include_dir}")
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "--sysroot=${CMAKE_SYSROOT} ${_cockscreen_startfile_prefix_flags} ${_cockscreen_rpath_link_flags} -L${_cockscreen_multiarch_library_dir} -L${_cockscreen_multiarch_system_library_dir}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT
    "--sysroot=${CMAKE_SYSROOT} ${_cockscreen_startfile_prefix_flags} ${_cockscreen_rpath_link_flags} -L${_cockscreen_multiarch_library_dir} -L${_cockscreen_multiarch_system_library_dir}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT
    "--sysroot=${CMAKE_SYSROOT} ${_cockscreen_startfile_prefix_flags} ${_cockscreen_rpath_link_flags} -L${_cockscreen_multiarch_library_dir} -L${_cockscreen_multiarch_system_library_dir}")

set(CMAKE_FIND_ROOT_PATH
    "${CMAKE_SYSROOT}"
)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

list(APPEND CMAKE_PREFIX_PATH
    "${CMAKE_SYSROOT}/usr"
    "${CMAKE_SYSROOT}/usr/lib/${_cockscreen_target_triplet}/cmake"
    "${CMAKE_SYSROOT}/usr/lib/cmake"
)

set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR}
    "${CMAKE_SYSROOT}/usr/lib/${_cockscreen_target_triplet}/pkgconfig:${CMAKE_SYSROOT}/usr/lib/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")

if(NOT DEFINED QT_HOST_PATH)
    set(QT_HOST_PATH "/usr" CACHE PATH "Host Qt installation used for moc, rcc, and other host tools")
endif()