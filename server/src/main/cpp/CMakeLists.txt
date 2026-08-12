cmake_minimum_required(VERSION 3.26.0)
project("scrcpy_native")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Link static Poco into this shared library
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Host protoc + protobuf runtime (for QtMirrorProto on Android cross-build)
# find_program must see host tools: NDK defaults may hide them.
# ---------------------------------------------------------------------------
if(ANDROID)
    set(_sc_native_saved_prog_mode "${CMAKE_FIND_ROOT_PATH_MODE_PROGRAM}")
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BOTH)
endif()

find_program(SC_HOST_PROTOC NAMES protoc protoc.exe
    HINTS
        "$ENV{VCPKG_ROOT}/installed/x64-windows/tools/protobuf"
        "$ENV{VCPKG_ROOT}/installed/x64-windows/bin"
    DOC "Host Protocol Buffers compiler (generates QtMirrorProto .pb.cc)")

if(ANDROID)
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM "${_sc_native_saved_prog_mode}")
    unset(_sc_native_saved_prog_mode)
endif()

if(NOT SC_HOST_PROTOC)
    message(FATAL_ERROR
        "protoc not found (host). Install protobuf compiler and ensure it is on PATH, "
        "or set VCPKG_ROOT. Example: vcpkg install protobuf:x64-windows")
endif()

set(WITH_PROTOC "${SC_HOST_PROTOC}" CACHE FILEPATH
    "Host protoc; used by protobuf/ and QtMirrorProto code generation" FORCE)
set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_CONFORMANCE OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_PROTOC_BINARIES OFF CACHE BOOL "" FORCE)
set(protobuf_WITH_ZLIB OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_LIBUPB OFF CACHE BOOL "" FORCE)
set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ABSL_USE_EXTERNAL_GOOGLETEST OFF CACHE BOOL "" FORCE)
# Pre-build abseil so protobuf sees TARGET absl::strings and skips fetch/add_subdirectory
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/protobuf/third_party/abseil-cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/abseil_build")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/protobuf"
    "${CMAKE_CURRENT_BINARY_DIR}/protobuf_build")

set(QTMIRROR_PROTOC_EXECUTABLE "${SC_HOST_PROTOC}")
add_subdirectory(deps/cpp_base_lib)
add_subdirectory(translator)
add_subdirectory(dep_message)
add_subdirectory(file_transfer)

# Poco: only Foundation + Net (WebSocket lives in Net)
set(POCO_MINIMAL_BUILD ON CACHE BOOL "Poco minimal" FORCE)
set(ENABLE_NET ON CACHE BOOL "" FORCE)
set(ENABLE_FASTLOGGER OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_SAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(poco)

add_library(${CMAKE_PROJECT_NAME} SHARED
    main.cpp
    jni_helper.cpp
    test_native.cpp
    http_server.cpp
    poco_websocket_server.cpp
    notification/notification_relay.cpp
)

find_library(log-lib log)

target_link_libraries(${CMAKE_PROJECT_NAME}
    PRIVATE
    ${log-lib}
    Poco::Net
    QtMirrorProto
    plugin_file_transfer
)

# Copy .so to standalone output
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_SOURCE_DIR}/../libs/${ANDROID_ABI}"
    COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>"
        "${CMAKE_SOURCE_DIR}/../libs/${ANDROID_ABI}/$<TARGET_FILE_NAME:${CMAKE_PROJECT_NAME}>"
    COMMENT "Copying .so to standalone libs/"
)
