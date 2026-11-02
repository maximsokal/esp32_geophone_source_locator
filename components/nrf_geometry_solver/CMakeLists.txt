idf_component_register(
    SRCS "nrf_geometry_solver.c" "nrf_geometry_triangles.c"
    INCLUDE_DIRS "include"
    REQUIRES nrf_cs_shared
)
