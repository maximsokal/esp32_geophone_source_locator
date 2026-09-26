set(NRF54L15_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../nRF54L15")

idf_component_register(
    SRCS
        "main_barometer.c"
        "${NRF54L15_ROOT}/src/nrf_baro_altitude.c"
        "${NRF54L15_ROOT}/src/nrf_ema_filter.c"

    INCLUDE_DIRS
        "include"
        "${NRF54L15_ROOT}/include"

    REQUIRES
        freertos
        i2c_bus
        sensor_bus
)
