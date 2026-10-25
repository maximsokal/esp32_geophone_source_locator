get_filename_component(NRF_BUILD_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(NRF_BUILD_INFO_HEADER "${CMAKE_CURRENT_BINARY_DIR}/nrf_build_info/nrf_build_info.h")
add_custom_target(nrf_build_info
    COMMAND ${CMAKE_COMMAND}
        "-DNRF_REPO_ROOT=${NRF_BUILD_REPO_ROOT}"
        "-DNRF_BUILD_INFO_OUTPUT=${NRF_BUILD_INFO_HEADER}"
        -P "${CMAKE_CURRENT_LIST_DIR}/write_build_info.cmake"
    BYPRODUCTS "${NRF_BUILD_INFO_HEADER}"
    VERBATIM
)
add_dependencies(app nrf_build_info)
target_include_directories(app PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/nrf_build_info")
