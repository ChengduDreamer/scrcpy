message(STATUS "file transmission(server) is enabled.")
project(plugin_file_transfer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PLUGIN_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/file_transfer_plugin.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/file_transmission_server/file_operate.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/file_transmission_server/file_transmit_impl.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/file_transmission_server/file_transmit_manager.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/file_transmission_server/file_transmit_msg_interface.cc
)

add_library(${PROJECT_NAME} STATIC ${PLUGIN_SRCS})
target_include_directories(${PROJECT_NAME} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../deps/cpp_base_lib
    ${CMAKE_CURRENT_SOURCE_DIR}/../deps/3rdparty/asio2/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../deps/3rdparty/asio2/3rd
)
target_link_libraries(${PROJECT_NAME} PUBLIC
    QtMirrorProto
    protobuf::libprotobuf
    yk_translator
)
