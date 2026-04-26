cmake_minimum_required(VERSION 3.22.1)
project("scrcpy_native")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(${CMAKE_PROJECT_NAME} SHARED
    main.cpp
    jni_helper.cpp
    test_native.cpp
)

find_library(log-lib log)

target_link_libraries(${CMAKE_PROJECT_NAME}
    ${log-lib}
)

# Copy .so to a standalone output directory (not bundled in APK)
# Output: server/src/main/libs/<abi>/libscrcpy_native.so
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_SOURCE_DIR}/../libs/${ANDROID_ABI}"
    COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>"
        "${CMAKE_SOURCE_DIR}/../libs/${ANDROID_ABI}/$<TARGET_FILE_NAME:${CMAKE_PROJECT_NAME}>"
    COMMENT "Copying .so to standalone libs/ directory"
)
