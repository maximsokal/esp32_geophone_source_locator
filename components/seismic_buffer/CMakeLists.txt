idf_component_register(
    SRCS
        "seismic_buffer.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        freertos
        heap
)
