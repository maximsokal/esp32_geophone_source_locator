idf_component_register(
    SRCS
        "wifi_qr.c"
        "wifi_qr_render.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        app_config
        display
        espressif__qrcode
        log
)
