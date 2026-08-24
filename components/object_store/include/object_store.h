#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"


#define OBJECT_STORE_ID_SIZE       16
#define OBJECT_STORE_NAME_SIZE     32
#define OBJECT_STORE_MAX_OBJECTS   32


typedef struct
{
    char id[
        OBJECT_STORE_ID_SIZE
    ];

    char name[
        OBJECT_STORE_NAME_SIZE
    ];

    bool active;

} object_store_summary_t;


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