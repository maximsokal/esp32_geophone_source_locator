idf_component_register(
    SRCS
        "seismic_buffer.c"
        "seismic_envelope.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        freertos
        heap
)
