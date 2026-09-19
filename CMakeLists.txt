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

if(GEOLOCATOR_KEYPAD_TIMING_PROFILE OR "$ENV{GEOLOCATOR_KEYPAD_TIMING_PROFILE}" STREQUAL "1")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.keypad-timing.defaults")
endif()

if(GEOLOCATOR_ADC_BENCH_PROFILE OR "$ENV{GEOLOCATOR_ADC_BENCH_PROFILE}" STREQUAL "1")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.adc-bench.defaults")
endif()

# The production project is pinned to ESP32-S3. ESP-TEE is an optional IDF
# component, but IDF 5.5 only supports it on C6/H2/C5 and otherwise prints a
# misleading configure-time notice while scanning every component.
list(APPEND EXCLUDE_COMPONENTS esp_tee)

# Espressif IDE does not always run the IDF export script before configuring
# the project. Supply the ROM ELF directory when it is discoverable so the
# bootloader configure step can generate gdbinit without a warning. The
# newest installed ROM ELF bundle is preferred when several are present.
if("$ENV{ESP_ROM_ELF_DIR}" STREQUAL "")
    set(_geolocator_tools_root "$ENV{IDF_TOOLS_PATH}")
    if("${_geolocator_tools_root}" STREQUAL "" AND NOT "$ENV{IDF_PATH}" STREQUAL "")
        get_filename_component(_geolocator_idf_frameworks "$ENV{IDF_PATH}" DIRECTORY)
        get_filename_component(_geolocator_tools_root "${_geolocator_idf_frameworks}" DIRECTORY)
    endif()
    if(NOT "${_geolocator_tools_root}" STREQUAL "")
        file(GLOB _geolocator_rom_elf_dirs LIST_DIRECTORIES true
            "${_geolocator_tools_root}/tools/esp-rom-elfs/*")
        if(_geolocator_rom_elf_dirs)
            list(SORT _geolocator_rom_elf_dirs ORDER DESCENDING)
            list(GET _geolocator_rom_elf_dirs 0 _geolocator_rom_elf_dir)
            set(ENV{ESP_ROM_ELF_DIR} "${_geolocator_rom_elf_dir}")
        endif()
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp32_geophone_source_locator)
