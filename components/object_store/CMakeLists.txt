idf_component_register(
    SRCS
        "object_store.c"
        "object_store_state.c"
        "object_store_restore.c"
        "object_store_operations.c"
        "object_store_mutations.c"
        "object_store_lease.c"
        "object_store_fs.c"
        "object_store_fs_ids.c"
        "object_store_fs_objects.c"
        "object_store_archive_policy.c"
        "object_store_fs_list.c"
        "object_store_fs_active.c"
        "object_store_delete_fs.c"
        "object_store_json.c"
        "object_store_profile.c"
        "object_store_geometry_json.c"
        "object_store_cache.c"
        "object_store_profile_cache_policy.c"
        "object_store_responsive.c"
        "object_store_artifacts.c"

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

    PRIV_REQUIRES
        cjson_alloc_guard
        heap
        json_number
)
