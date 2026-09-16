#include "seismic_recorder_session_selection.h"

#include <string.h>


bool seismic_recorder_session_id_parse(
    const char *name,
    unsigned *number_out
)
{
    if (name == NULL) {
        return false;
    }

    static const size_t canonical_length = sizeof("S000000") - 1U;
    if (strlen(name) != canonical_length || name[0] != 'S') {
        return false;
    }

    unsigned parsed = 0U;
    for (size_t index = 1U; index < canonical_length; ++index) {
        const char digit = name[index];
        if (digit < '0' || digit > '9') {
            return false;
        }
        parsed = parsed * 10U + (unsigned)(digit - '0');
    }

    if (parsed == 0U || parsed > 999999U) {
        return false;
    }

    if (number_out != NULL) {
        *number_out = parsed;
    }
    return true;
}


void seismic_recorder_session_selection_init(
    seismic_recorder_session_selection_t *selection,
    size_t requested_capacity
)
{
    if (selection == NULL) {
        return;
    }

    memset(selection, 0, sizeof(*selection));
    selection->capacity = requested_capacity < SEISMIC_RECORDER_MAX_SESSIONS
        ? requested_capacity
        : SEISMIC_RECORDER_MAX_SESSIONS;
}


void seismic_recorder_session_selection_consider(
    seismic_recorder_session_selection_t *selection,
    const char *session_id
)
{
    if (
        selection == NULL ||
        selection->capacity == 0U ||
        !seismic_recorder_session_id_parse(session_id, NULL)
    ) {
        return;
    }

    size_t position = 0U;
    while (
        position < selection->count &&
        strcmp(selection->ids[position], session_id) > 0
    ) {
        ++position;
    }

    if (
        position < selection->count &&
        strcmp(selection->ids[position], session_id) == 0
    ) {
        return;
    }

    if (position >= selection->capacity) {
        return;
    }

    size_t new_count = selection->count;
    if (new_count < selection->capacity) {
        ++new_count;
    }

    for (size_t index = new_count; index > position + 1U; --index) {
        memcpy(
            selection->ids[index - 1U],
            selection->ids[index - 2U],
            sizeof(selection->ids[0])
        );
    }

    static const size_t canonical_size = sizeof("S000000");
    memset(selection->ids[position], 0, sizeof(selection->ids[position]));
    memcpy(selection->ids[position], session_id, canonical_size);
    selection->count = new_count;
}


size_t seismic_recorder_session_selection_count(
    const seismic_recorder_session_selection_t *selection
)
{
    return selection != NULL ? selection->count : 0U;
}


const char *seismic_recorder_session_selection_id(
    const seismic_recorder_session_selection_t *selection,
    size_t index
)
{
    if (selection == NULL || index >= selection->count) {
        return NULL;
    }
    return selection->ids[index];
}
