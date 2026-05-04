cmake_minimum_required(VERSION 3.26.0)
project("scrcpy_native")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Link static Poco into this shared library
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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
)

find_library(log-lib log)

target_link_libraries(${CMAKE_PROJECT_NAME}
    ${log-lib}
    Poco::Net
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
