idf_component_register(
    SRCS
        "wifi_qr.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        app_config
        display
        espressif__qrcode
        log
)