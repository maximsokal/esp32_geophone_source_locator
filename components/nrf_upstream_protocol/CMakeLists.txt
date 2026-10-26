# One canonical wire codec is shared by Zephyr/nRF and ESP-IDF/ESP32.
# Keep the source in nRF54L15 because pio.ps1 SUBST-maps exactly that project.
set(NRF_UPSTREAM_PROTOCOL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../nRF54L15")

idf_component_register(
    SRCS
        "${NRF_UPSTREAM_PROTOCOL_ROOT}/src/nrf_upstream_protocol.c"
        "${NRF_UPSTREAM_PROTOCOL_ROOT}/src/nrf_log_protocol.c"
        "${NRF_UPSTREAM_PROTOCOL_ROOT}/src/nrf_read_window.c"

    INCLUDE_DIRS
        "${NRF_UPSTREAM_PROTOCOL_ROOT}/include"
)
