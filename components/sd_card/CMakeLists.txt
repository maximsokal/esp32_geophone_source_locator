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
        freertos
        heap
        log
)
