#pragma once

#include "esp_err.h"


/*
 * Build the current device status as allocated JSON.
 */
esp_err_t web_status_json_build(
    char **json_out
);


/*
 * Free JSON returned by web_status_json_build().
 */
void web_status_json_free(
    char *json
);