idf_component_register(
    SRCS
        "object_store.c"
        "object_store_state.c"
        "object_store_restore.c"
        "object_store_operations.c"
        "object_store_mutations.c"
        "object_store_fs.c"
        "object_store_fs_ids.c"
        "object_store_fs_objects.c"
        "object_store_fs_list.c"
        "object_store_fs_active.c"
        "object_store_delete_fs.c"
        "object_store_json.c"
        "object_store_profile.c"
        "object_store_responsive.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "."

    REQUIRES
        object_format
        storage_manager
        freertos
        json
        log
)
