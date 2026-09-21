idf_component_register(
    SRCS
        "localization_run_store.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        localization_results
        object_format
        storage_manager
)
