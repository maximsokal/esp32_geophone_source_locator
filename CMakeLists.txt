cmake_minimum_required(VERSION 3.16)

set(
    SDKCONFIG_DEFAULTS
    "sdkconfig.defaults;sdkconfig.storage.defaults;sdkconfig.lvgl.defaults;sdkconfig.tls.defaults;sdkconfig.button.defaults"
)

# The optional ESP Insights build is an explicit, isolated profile. It must
# never turn on in the normal production defaults or export diagnostics data
# before the Wi-Fi platform exists.
if(GEOLOCATOR_INSIGHTS_PROFILE OR "$ENV{GEOLOCATOR_INSIGHTS_PROFILE}" STREQUAL "1")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.insights.defaults")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp32_geophone_source_locator)
