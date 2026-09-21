#pragma once

#include <stddef.h>

#include "esp_err.h"

#define SEISMIC_RECORDER_SESSION_ID_SIZE 16
#define SEISMIC_RECORDER_MAX_SESSIONS    32

/* Only the field object_recovery_migrate_legacy_scan() actually reads. */
typedef struct {
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE];
} seismic_recorder_session_summary_t;

esp_err_t seismic_recorder_list_sessions(
    const char *object_id,
    seismic_recorder_session_summary_t *sessions,
    size_t max_sessions,
    size_t *session_count
);
