cmake_minimum_required(VERSION 3.21)

project(pb2_locator
    VERSION 0.1.0
    DESCRIPTION "PocketBeagle 2 Rev A0 seismic acquisition, DSP, and host-offload node"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PB2_BUILD_TESTS "Build PocketBeagle2 unit tests" ON)

add_library(pb2_core STATIC
    src/acquisition/mock_source.cpp
    src/diagnostics/runtime_info.cpp
)

target_include_directories(pb2_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(pb2_core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

add_executable(pb2_node
    src/main.cpp
)

target_link_libraries(pb2_node PRIVATE pb2_core)

target_compile_options(pb2_node PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

if(PB2_BUILD_TESTS)
    enable_testing()

    add_executable(pb2_unit_tests
        tests/unit/test_mock_source.cpp
    )

    target_link_libraries(pb2_unit_tests PRIVATE pb2_core)

    target_compile_options(pb2_unit_tests PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
    )

    add_test(NAME pb2_unit_tests COMMAND pb2_unit_tests)
endif()
