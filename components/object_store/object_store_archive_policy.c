#include "object_store_archive_policy.h"

#include <stdio.h>
#include <string.h>


static void object_store_archive_selection_sort(
    object_store_archive_selection_t *selection
)
{
    for (size_t i = 0U; i < selection->count; ++i) {
        for (size_t j = i + 1U; j < selection->count; ++j) {
            if (strcmp(selection->ids[i], selection->ids[j]) <= 0) {
                continue;
            }

            char temp[OBJECT_FORMAT_ID_SIZE];
            memcpy(temp, selection->ids[i], sizeof(temp));
            memcpy(selection->ids[i], selection->ids[j], sizeof(temp));
            memcpy(selection->ids[j], temp, sizeof(temp));
        }
    }
}


static bool object_store_archive_selection_contains(
    const object_store_archive_selection_t *selection,
    const char *object_id
)
{
    for (size_t i = 0U; i < selection->count; ++i) {
        if (strcmp(selection->ids[i], object_id) == 0) {
            return true;
        }
    }
    return false;
}


static void object_store_archive_selection_add(
    object_store_archive_selection_t *selection,
    const char *object_id
)
{
    snprintf(
        selection->ids[selection->count++],
        OBJECT_FORMAT_ID_SIZE,
        "%s",
        object_id
    );
    object_store_archive_selection_sort(selection);
}


void object_store_archive_selection_init(
    object_store_archive_selection_t *selection,
    size_t capacity,
    const char *active_object_id
)
{
    if (selection == NULL) {
        return;
    }

    memset(selection, 0, sizeof(*selection));
    selection->capacity = capacity < OBJECT_FORMAT_MAX_OBJECTS
        ? capacity
        : OBJECT_FORMAT_MAX_OBJECTS;

    if (active_object_id != NULL && active_object_id[0] != '\0') {
        snprintf(
            selection->active_id,
            sizeof(selection->active_id),
            "%s",
            active_object_id
        );
    }
}


void object_store_archive_selection_consider(
    object_store_archive_selection_t *selection,
    const char *object_id
)
{
    if (
        selection == NULL ||
        object_id == NULL ||
        object_id[0] == '\0' ||
        selection->capacity == 0U
    ) {
        return;
    }

    if (
        selection->active_id[0] != '\0' &&
        strcmp(selection->active_id, object_id) == 0
    ) {
        selection->active_seen = true;
    }

    if (object_store_archive_selection_contains(selection, object_id)) {
        return;
    }

    if (selection->count < selection->capacity) {
        object_store_archive_selection_add(selection, object_id);
        return;
    }

    if (strcmp(object_id, selection->ids[0]) <= 0) {
        return;
    }

    snprintf(
        selection->ids[0],
        sizeof(selection->ids[0]),
        "%s",
        object_id
    );
    object_store_archive_selection_sort(selection);
}


void object_store_archive_selection_finalize(
    object_store_archive_selection_t *selection
)
{
    if (
        selection == NULL ||
        !selection->active_seen ||
        selection->active_id[0] == '\0' ||
        object_store_archive_selection_contains(
            selection,
            selection->active_id
        )
    ) {
        return;
    }

    if (selection->count < selection->capacity) {
        object_store_archive_selection_add(
            selection,
            selection->active_id
        );
        return;
    }

    if (selection->count == 0U) {
        return;
    }

    /* ACTIVE is part of the public bounded snapshot even when it is older
     * than the ordinary top-N set. Replace that set's oldest member. */
    snprintf(
        selection->ids[0],
        sizeof(selection->ids[0]),
        "%s",
        selection->active_id
    );
    object_store_archive_selection_sort(selection);
}


size_t object_store_archive_selection_count(
    const object_store_archive_selection_t *selection
)
{
    return selection != NULL ? selection->count : 0U;
}


const char *object_store_archive_selection_id(
    const object_store_archive_selection_t *selection,
    size_t index
)
{
    return selection != NULL && index < selection->count
        ? selection->ids[index]
        : NULL;
}


bool object_store_archive_selection_is_active(
    const object_store_archive_selection_t *selection,
    const char *object_id
)
{
    return
        selection != NULL &&
        object_id != NULL &&
        selection->active_seen &&
        selection->active_id[0] != '\0' &&
        strcmp(selection->active_id, object_id) == 0;
}
