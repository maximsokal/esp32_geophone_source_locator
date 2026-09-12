idf_component_register(
    SRCS
        "test_app_main.c"
        "test_app_contracts.c"
        "../../../../components/ad7606_driver/ad7606_presence_policy.c"
        "../../../../components/localization/localization.c"
        "../../../../components/object_format/object_format.c"
        "../../../../components/ui/ui_measurement_trace.c"

    INCLUDE_DIRS
        "."

    PRIV_INCLUDE_DIRS
        "../../../../components/ad7606_driver"
        "../../../../components/localization/include"
        "../../../../components/object_format/include"
        "../../../../components/seismic_buffer/include"
        "../../../../components/ui"

    PRIV_REQUIRES unity
    WHOLE_ARCHIVE
)
