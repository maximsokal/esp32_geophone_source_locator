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

# The emulator profile disables ADC-backed keypad polling. Keep direct CMake
# selection isolated so it cannot replace the normal build's sdkconfig.
if(GEOLOCATOR_EMULATOR_PROFILE OR "$ENV{GEOLOCATOR_EMULATOR_PROFILE}" STREQUAL "1")
    get_filename_component(_geolocator_emulator_build_dir "${CMAKE_BINARY_DIR}" ABSOLUTE)
    get_filename_component(_geolocator_emulator_test_root "${CMAKE_SOURCE_DIR}/.test-build" ABSOLUTE)
    file(TO_CMAKE_PATH "${_geolocator_emulator_build_dir}" _geolocator_emulator_build_dir)
    file(TO_CMAKE_PATH "${_geolocator_emulator_test_root}" _geolocator_emulator_test_root)
    string(FIND
        "${_geolocator_emulator_build_dir}/"
        "${_geolocator_emulator_test_root}/"
        _geolocator_emulator_build_prefix_index
    )
    if(NOT _geolocator_emulator_build_prefix_index EQUAL 0 OR
        "${_geolocator_emulator_build_dir}" STREQUAL "${_geolocator_emulator_test_root}")
        message(FATAL_ERROR "The emulator profile requires a separate build directory under .test-build/.")
    endif()
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.emulator.defaults")
endif()

# The watchdog-trigger profile is emulator-only and extends the emulator
# defaults in order. Keep direct CMake selection isolated from build/ as well.
if(GEOLOCATOR_EMULATOR_WDT_PROFILE OR "$ENV{GEOLOCATOR_EMULATOR_WDT_PROFILE}" STREQUAL "1")
    get_filename_component(_geolocator_emulator_wdt_build_dir "${CMAKE_BINARY_DIR}" ABSOLUTE)
    get_filename_component(_geolocator_emulator_wdt_test_root "${CMAKE_SOURCE_DIR}/.test-build" ABSOLUTE)
    file(TO_CMAKE_PATH "${_geolocator_emulator_wdt_build_dir}" _geolocator_emulator_wdt_build_dir)
    file(TO_CMAKE_PATH "${_geolocator_emulator_wdt_test_root}" _geolocator_emulator_wdt_test_root)
    string(FIND
        "${_geolocator_emulator_wdt_build_dir}/"
        "${_geolocator_emulator_wdt_test_root}/"
        _geolocator_emulator_wdt_build_prefix_index
    )
    if(NOT _geolocator_emulator_wdt_build_prefix_index EQUAL 0 OR
        "${_geolocator_emulator_wdt_build_dir}" STREQUAL "${_geolocator_emulator_wdt_test_root}")
        message(FATAL_ERROR "The emulator-wdt profile requires a separate build directory under .test-build/.")
    endif()
    list(FIND SDKCONFIG_DEFAULTS "sdkconfig.emulator.defaults" _geolocator_emulator_defaults_index)
    if(_geolocator_emulator_defaults_index EQUAL -1)
        list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.emulator.defaults")
    endif()
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.emulator-wdt.defaults")
endif()

# The recording-scenario profile (S10) is emulator-only as well: the emulator defaults, then its own.
if(GEOLOCATOR_EMULATOR_REC_PROFILE OR "$ENV{GEOLOCATOR_EMULATOR_REC_PROFILE}" STREQUAL "1")
    get_filename_component(_geolocator_emulator_rec_build_dir "${CMAKE_BINARY_DIR}" ABSOLUTE)
    get_filename_component(_geolocator_emulator_rec_test_root "${CMAKE_SOURCE_DIR}/.test-build" ABSOLUTE)
    file(TO_CMAKE_PATH "${_geolocator_emulator_rec_build_dir}" _geolocator_emulator_rec_build_dir)
    file(TO_CMAKE_PATH "${_geolocator_emulator_rec_test_root}" _geolocator_emulator_rec_test_root)
    string(FIND
        "${_geolocator_emulator_rec_build_dir}/"
        "${_geolocator_emulator_rec_test_root}/"
        _geolocator_emulator_rec_build_prefix_index
    )
    if(NOT _geolocator_emulator_rec_build_prefix_index EQUAL 0 OR
        "${_geolocator_emulator_rec_build_dir}" STREQUAL "${_geolocator_emulator_rec_test_root}")
        message(FATAL_ERROR "The emulator-rec profile requires a separate build directory under .test-build/.")
    endif()
    list(FIND SDKCONFIG_DEFAULTS "sdkconfig.emulator.defaults" _geolocator_emulator_rec_defaults_index)
    if(_geolocator_emulator_rec_defaults_index EQUAL -1)
        list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.emulator.defaults")
    endif()
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.emulator-rec.defaults")
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
