#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "object_format.h"


typedef struct
{
    size_t capacity;
    size_t count;
    bool active_seen;
    char active_id[OBJECT_FORMAT_ID_SIZE];
    char ids[OBJECT_FORMAT_MAX_OBJECTS][OBJECT_FORMAT_ID_SIZE];

} object_store_archive_selection_t;


void object_store_archive_selection_init(
    object_store_archive_selection_t *selection,
    size_t capacity,
    const char *active_object_id
);

void object_store_archive_selection_consider(
    object_store_archive_selection_t *selection,
    const char *object_id
);

void object_store_archive_selection_finalize(
    object_store_archive_selection_t *selection
);

size_t object_store_archive_selection_count(
    const object_store_archive_selection_t *selection
);

const char *object_store_archive_selection_id(
    const object_store_archive_selection_t *selection,
    size_t index
);

bool object_store_archive_selection_is_active(
    const object_store_archive_selection_t *selection,
    const char *object_id
);
