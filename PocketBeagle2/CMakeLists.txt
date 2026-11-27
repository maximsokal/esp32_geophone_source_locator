cmake_minimum_required(VERSION 3.21)

project(pb2_locator
    VERSION 0.1.0
    DESCRIPTION "PocketBeagle 2 Rev A0 seismic acquisition, DSP, and host-offload node"
    LANGUAGES C CXX
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PB2_BUILD_TESTS "Build PocketBeagle2 unit and PTY integration tests" ON)

set(PB2_SHARED_PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../components/pb2_protocol")

add_library(pb2_protocol STATIC
    "${PB2_SHARED_PROTO_DIR}/pb2_cobs.c"
    "${PB2_SHARED_PROTO_DIR}/pb2_crc32c.c"
    "${PB2_SHARED_PROTO_DIR}/pb2_protocol.c"
)

target_include_directories(pb2_protocol
    PUBLIC
        "${PB2_SHARED_PROTO_DIR}/include"
)

target_compile_options(pb2_protocol PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

add_library(pb2_core STATIC
    src/acquisition/mock_source.cpp
    src/diagnostics/runtime_info.cpp
    src/transport/uart_transport.cpp
)

target_include_directories(pb2_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(pb2_core
    PUBLIC
        pb2_protocol
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
    target_compile_options(pb2_unit_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME pb2_unit_tests COMMAND pb2_unit_tests)

    add_executable(pb2_protocol_c_tests
        "${PB2_SHARED_PROTO_DIR}/tests/pb2_protocol_test.c"
    )
    target_link_libraries(pb2_protocol_c_tests PRIVATE pb2_protocol m)
    target_compile_options(pb2_protocol_c_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME pb2_protocol_c_tests COMMAND pb2_protocol_c_tests)

    add_executable(pb2_pty_tests
        tests/integration/test_pty_transport.cpp
    )
    target_link_libraries(pb2_pty_tests PRIVATE pb2_core util)
    target_compile_options(pb2_pty_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME pb2_pty_tests COMMAND pb2_pty_tests)
endif()
