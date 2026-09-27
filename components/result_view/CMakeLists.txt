idf_component_register(
    SRCS "result_view.c"
    INCLUDE_DIRS "include"
    REQUIRES geophone_events object_store
)

target_link_libraries(${COMPONENT_LIB} PRIVATE m)
