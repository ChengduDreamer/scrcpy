project(yk_translator)

if(ANDROID)
    add_library(${PROJECT_NAME}
        yk_translator.cpp
    )
    target_include_directories(${PROJECT_NAME}
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}"
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/../deps"
            "${CMAKE_CURRENT_SOURCE_DIR}/../deps/3rdparty"
    )
else()
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTOUIC ON)
    set(CMAKE_AUTORCC ON)

    find_package(Qt6 REQUIRED COMPONENTS Widgets Core)

    add_library(${PROJECT_NAME}
            yk_translator.cpp
    )

    target_include_directories(${PROJECT_NAME}
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}"
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/../deps"
            "${CMAKE_CURRENT_SOURCE_DIR}/../deps/3rdparty"
    )

    target_link_libraries(${PROJECT_NAME}
        PRIVATE
            yk_base_lib
    )
endif()
