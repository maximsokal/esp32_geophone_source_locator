idf_component_register(
    SRCS
        "storage_manager.c"
        "storage_bulk_admission.c"
        "storage_manager_config_check.c"
        "storage_manager_directory.c"
        "storage_manager_text_helpers.c"
        "storage_manager_text_write.c"
        "storage_manager_text_read.c"
        "storage_manager_tree.c"
        "storage_manager_erase.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        freertos
        esp_timer
        esp_system
        log
        spi_bus
)
