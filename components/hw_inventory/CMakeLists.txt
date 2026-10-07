idf_component_register(
    SRCS
        "hw_inventory.c"
        "hw_inventory_reason.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_common
        freertos
        log
)
