# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.13.1)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(geophone_nrf54l15_node)

target_sources(app PRIVATE
    ../src/main.c
    ../src/nrf_node_state.c
    ../src/nrf_sensor_bus.c
    ../src/nrf_sensor_snapshot.c
)

target_include_directories(app PRIVATE ../include)
