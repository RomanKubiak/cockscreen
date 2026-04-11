if(NOT DEFINED COCKSCREEN_PI_SYNC_HOST)
    message(FATAL_ERROR "COCKSCREEN_PI_SYNC_HOST is required")
endif()

if(NOT DEFINED COCKSCREEN_PI_SYNC_ROOT)
    message(FATAL_ERROR "COCKSCREEN_PI_SYNC_ROOT is required")
endif()

if(NOT DEFINED COCKSCREEN_PI_SYNC_BINARY)
    message(FATAL_ERROR "COCKSCREEN_PI_SYNC_BINARY is required")
endif()

if(NOT DEFINED COCKSCREEN_PI_SYNC_BUILD_DIR)
    message(FATAL_ERROR "COCKSCREEN_PI_SYNC_BUILD_DIR is required")
endif()

find_program(SSH_EXECUTABLE ssh REQUIRED)
find_program(RSYNC_EXECUTABLE rsync REQUIRED)

get_filename_component(_build_dir_name "${COCKSCREEN_PI_SYNC_BUILD_DIR}" NAME)
set(_remote_build_dir "${COCKSCREEN_PI_SYNC_ROOT}/out/build/${_build_dir_name}")
get_filename_component(_binary_name "${COCKSCREEN_PI_SYNC_BINARY}" NAME)
set(_remote_binary "${_remote_build_dir}/${_binary_name}")

execute_process(
    COMMAND
        "${SSH_EXECUTABLE}"
        "${COCKSCREEN_PI_SYNC_HOST}"
        "mkdir -p '${_remote_build_dir}'"
    RESULT_VARIABLE _mkdir_result
)

if(NOT _mkdir_result EQUAL 0)
    message(FATAL_ERROR "Failed to create remote build directory: ${_remote_build_dir}")
endif()

execute_process(
    COMMAND
        "${RSYNC_EXECUTABLE}"
        -az
        --delete
        "${COCKSCREEN_PI_SYNC_BINARY}"
        "${COCKSCREEN_PI_SYNC_HOST}:${_remote_binary}"
    RESULT_VARIABLE _rsync_result
)

if(NOT _rsync_result EQUAL 0)
    message(FATAL_ERROR "Failed to sync ${COCKSCREEN_PI_SYNC_BINARY} to ${_remote_binary}")
endif()

message(STATUS "Synced ${COCKSCREEN_PI_SYNC_BINARY} to ${COCKSCREEN_PI_SYNC_HOST}:${_remote_binary}")