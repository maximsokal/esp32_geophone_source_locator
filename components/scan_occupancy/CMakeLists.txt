idf_component_register(
    SRCS
        "scan_occupancy.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        json
)

target_link_libraries(${COMPONENT_LIB} PRIVATE m)
