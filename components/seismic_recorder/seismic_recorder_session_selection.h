#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "seismic_recorder.h"


typedef struct
{
    size_t capacity;
    size_t count;
    char ids[SEISMIC_RECORDER_MAX_SESSIONS]
            [SEISMIC_RECORDER_SESSION_ID_SIZE];

} seismic_recorder_session_selection_t;


bool seismic_recorder_session_id_parse(
    const char *name,
    unsigned *number_out
);

void seismic_recorder_session_selection_init(
    seismic_recorder_session_selection_t *selection,
    size_t requested_capacity
);

void seismic_recorder_session_selection_consider(
    seismic_recorder_session_selection_t *selection,
    const char *session_id
);

size_t seismic_recorder_session_selection_count(
    const seismic_recorder_session_selection_t *selection
);

const char *seismic_recorder_session_selection_id(
    const seismic_recorder_session_selection_t *selection,
    size_t index
);
