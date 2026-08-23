#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"


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


/*
 * Install a custom HTTP 404 handler.
 */
esp_err_t web_server_set_not_found_handler(
    httpd_err_handler_func_t handler
);


/*
 * Restore the default HTTP 404 behavior.
 */
esp_err_t web_server_clear_not_found_handler(void);