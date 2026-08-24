#pragma once

#include <stddef.h>

#include "esp_err.h"

#include "object_format.h"


#define OBJECT_STORE_ID_SIZE \
    OBJECT_FORMAT_ID_SIZE

#define OBJECT_STORE_NAME_SIZE \
    OBJECT_FORMAT_NAME_SIZE

#define OBJECT_STORE_MAX_OBJECTS \
    OBJECT_FORMAT_MAX_OBJECTS


typedef object_summary_t
    object_store_summary_t;


esp_err_t object_store_init(void);


esp_err_t object_store_create(
    object_store_summary_t *created_object
);


esp_err_t object_store_list(
    object_store_summary_t *objects,
    size_t max_objects,
    size_t *object_count
);


esp_err_t object_store_get_active(
    object_store_summary_t *object
);


esp_err_t object_store_set_active(
    const char *object_id
);


esp_err_t object_store_delete(
    const char *object_id
);


esp_err_t object_store_reset(void);
