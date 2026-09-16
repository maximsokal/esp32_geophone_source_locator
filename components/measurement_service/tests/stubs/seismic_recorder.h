#pragma once

#define seismic_recorder_stop_stage_name measurement_service_test_stop_stage_name
#include "../../../seismic_recorder/include/seismic_recorder.h"

const char *measurement_service_test_stop_stage_name(
    seismic_recorder_stop_stage_t stage
)
{
    switch (stage) {
        case SEISMIC_RECORDER_STOP_STAGE_DRAIN:
            return "drain";
        case SEISMIC_RECORDER_STOP_STAGE_FLUSH:
            return "flush";
        case SEISMIC_RECORDER_STOP_STAGE_SYNC:
            return "sync";
        case SEISMIC_RECORDER_STOP_STAGE_CLOSE:
            return "close";
        case SEISMIC_RECORDER_STOP_STAGE_METADATA:
            return "metadata";
        case SEISMIC_RECORDER_STOP_STAGE_COMPLETE:
            return "complete";
        case SEISMIC_RECORDER_STOP_STAGE_ERROR:
            return "error";
        case SEISMIC_RECORDER_STOP_STAGE_NONE:
        default:
            return "none";
    }
}
