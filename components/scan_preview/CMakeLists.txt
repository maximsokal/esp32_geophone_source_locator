idf_component_register(
    SRCS "scan_preview.c" "scan_preview_state.c"
    INCLUDE_DIRS "include"
    REQUIRES scan_occupancy
    PRIV_REQUIRES heap
)
