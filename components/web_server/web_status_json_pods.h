#pragma once

#include <stdbool.h>

#include "cJSON.h"

#include "device_status.h"


/*
 * Add the four pod objects to the JSON root.
 */
bool web_status_json_add_pods(
    cJSON *root,
    const device_status_snapshot_t *status
);