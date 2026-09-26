#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define SEISMIC_RECORDER_MAX_SESSIONS 32U
#define SEISMIC_RECORDER_SESSION_ID_SIZE 16U
#define SEISMIC_RECORDER_OBJECT_ID_SIZE 16U

typedef struct {
    bool running;
    char object_id[SEISMIC_RECORDER_OBJECT_ID_SIZE];
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE];
} seismic_recorder_snapshot_t;

typedef struct {
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE];
} seismic_recorder_session_summary_t;

esp_err_t seismic_recorder_list_sessions(
    const char *object_id,
    seismic_recorder_session_summary_t *sessions,
    size_t max_sessions,
    size_t *session_count
);

esp_err_t seismic_recorder_get_snapshot(seismic_recorder_snapshot_t *snapshot);
