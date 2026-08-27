idf_component_register(
    SRCS
        "adc_service.c"
        "adc_service_channel.c"
        "adc_service_read.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        esp_adc
        esp_driver_gpio
        freertos
        log
)
