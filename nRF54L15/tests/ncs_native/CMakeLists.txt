cmake_minimum_required(VERSION 3.20.0)

if(NCS_NATIVE_CS)
    set(CONF_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../../zephyr/prj.conf")
    set(EXTRA_CONF_FILE "${CMAKE_CURRENT_SOURCE_DIR}/prj.cs.conf")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nrf54l15_ncs_native)

target_sources(app PRIVATE
    src/main.c
    ../../src/nrf_node_state.c
    ../../src/nrf_sensor_bus.c
    ../../src/nrf_sensor_bus_zephyr.c
    ../../src/nrf_bmi160.c
    ../../src/nrf_bmi160_zephyr.c
    ../../src/nrf_bme280.c
    ../../src/nrf_bme280_zephyr.c
    ../../src/nrf_bme280_altitude.c
    ../../src/nrf_sensor_snapshot.c
    ../../src/nrf_remote_sensor_runtime.c
    ../../src/nrf_remote_sensor_runtime_zephyr.c
    ../../src/nrf_cs_scheduler.c
    ../../src/nrf_cs_pair_adapter.c
    ../../src/nrf_cs_pair_runtime_zephyr.c
)

if(CONFIG_BOARD_NATIVE_SIM)
    target_sources(app PRIVATE src/fake_i2c.c)
endif()

target_include_directories(app PRIVATE ../../include)
