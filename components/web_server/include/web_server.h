#pragma once

#include <stdbool.h>

#include "esp_err.h"


/*
 * Start the local HTTP server.
 */
esp_err_t web_server_start(void);


/*
 * Stop the local HTTP server.
 */
esp_err_t web_server_stop(void);


/*
 * Return true while the HTTP server is running.
 */
bool web_server_is_running(void);