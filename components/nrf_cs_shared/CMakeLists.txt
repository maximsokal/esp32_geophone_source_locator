# Canonical MAX-49/MAX-97 scheduler + control wire codec are shared with nRF.
set(NRF_CS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../nRF54L15")

idf_component_register(
    SRCS
        "${NRF_CS_ROOT}/src/nrf_cs_scheduler.c"
        "${NRF_CS_ROOT}/src/nrf_cs_control_protocol.c"
    INCLUDE_DIRS
        "${NRF_CS_ROOT}/include"
    REQUIRES
        nrf_upstream_protocol
)
