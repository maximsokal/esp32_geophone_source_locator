#pragma once
#include <stdbool.h>
#include "esp_err.h"
#define SEISMIC_RECORDER_SESSION_ID_SIZE 32
#define SEISMIC_RECORDER_OBJECT_ID_SIZE 32
typedef struct {
    bool running;
    int state;
    char object_id[SEISMIC_RECORDER_OBJECT_ID_SIZE];
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE];
} seismic_recorder_snapshot_t;
enum { SEISMIC_RECORDER_RECORDING = 1 };
esp_err_t seismic_recorder_get_snapshot(seismic_recorder_snapshot_t *snapshot);
esp_err_t seismic_recorder_stop(void);
