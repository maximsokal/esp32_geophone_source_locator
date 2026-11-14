set(SD_CARD_TEST_SOURCES)
if(CONFIG_APP_TEST_RECORDING_SCENARIO)
    list(APPEND SD_CARD_TEST_SOURCES "sd_card_test_flash.c")
endif()

idf_component_register(
    SRCS
        "sd_card.c"
        "sd_card_state.c"
        "sd_card_spi.c"
        "sd_card_mount.c"
        "sd_card_mount_recovery.c"
        "sd_card_format.c"
        "sd_card_info.c"
        "sd_card_diagnostics.c"
        "sd_card_diagnostics_io.c"
        ${SD_CARD_TEST_SOURCES}

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        board_config
        app_config
        fatfs
        sdmmc
        esp_driver_gpio
        esp_driver_sdspi
        esp_system
        wear_levelling
        esp_partition
        freertos
        heap
        log
)
