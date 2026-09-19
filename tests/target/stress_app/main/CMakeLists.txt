idf_component_register(
    SRCS
        "stress_app_main.c"
        "stress_physical_probe.c"
        "../../../../components/seismic_buffer/seismic_buffer.c"
        "../../../../components/seismic_recorder/seismic_recorder_cache.c"

    INCLUDE_DIRS
        "."

    PRIV_INCLUDE_DIRS
        "../../../../components/ad7606_driver/include"
        "../../../../components/display/include"
        "../../../../components/sd_card/include"
        "../../../../components/sensor_bus/include"
        "../../../../components/spi_bus/include"

    PRIV_INCLUDE_DIRS
        "../../../../components/seismic_buffer/include"
        "../../../../components/seismic_recorder"

    REQUIRES
        ad7606_driver
        board_config
        display
        esp_event
        esp_netif
        esp_wifi
        freertos
        i2c_bus
        esp_timer
        fatfs
        heap
        log
        nvs_flash
        sd_card
        sdmmc
        sensor_bus
        spi_bus
        esp_psram
        esp_system
)
