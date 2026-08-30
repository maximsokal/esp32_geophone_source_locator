cmake_minimum_required(VERSION 3.16)

set(
    SDKCONFIG_DEFAULTS
    "sdkconfig.defaults;sdkconfig.storage.defaults;sdkconfig.lvgl.defaults;sdkconfig.tls.defaults"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp32_geophone_source_locator)
