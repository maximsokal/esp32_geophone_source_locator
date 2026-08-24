idf_component_register(
    SRCS
        "object_store.c"
        "object_store_fs.c"
        "object_store_delete_fs.c"
        "object_store_json.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        storage_manager
        freertos
        json
        log
)