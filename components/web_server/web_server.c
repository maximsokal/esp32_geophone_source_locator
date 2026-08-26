#include "web_server.h"

#include <stddef.h>

#include "esp_log.h"

#include "web_server_routes.h"


static const char *TAG =
    "WEB_SERVER";


static httpd_handle_t s_server =
    NULL;


esp_err_t web_server_start(void)
{
    if (s_server != NULL) {
        ESP_LOGW(
            TAG,
            "HTTP server already running"
        );

        return ESP_OK;
    }

    httpd_config_t config =
        HTTPD_DEFAULT_CONFIG();

    config.max_uri_handlers =
        14;

    config.lru_purge_enable =
        true;

    ESP_LOGI(
        TAG,
        "Starting HTTP server on port %d",
        config.server_port
    );

    esp_err_t err =
        httpd_start(
            &s_server,
            &config
        );

    if (err != ESP_OK) {
        s_server =
            NULL;

        ESP_LOGE(
            TAG,
            "Failed to start HTTP server: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    err =
        web_server_routes_register(
            s_server
        );

    if (err != ESP_OK) {
        web_server_stop();

        return err;
    }

    ESP_LOGI(
        TAG,
        "HTTP server started"
    );

    return ESP_OK;
}


esp_err_t web_server_set_not_found_handler(
    httpd_err_handler_func_t handler
)
{
    if (s_server == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return
        httpd_register_err_handler(
            s_server,
            HTTPD_404_NOT_FOUND,
            handler
        );
}


esp_err_t web_server_clear_not_found_handler(void)
{
    if (s_server == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return
        httpd_register_err_handler(
            s_server,
            HTTPD_404_NOT_FOUND,
            NULL
        );
}


esp_err_t web_server_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }

    ESP_LOGI(
        TAG,
        "Stopping HTTP server"
    );

    esp_err_t err =
        httpd_stop(
            s_server
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to stop HTTP server: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    s_server =
        NULL;

    ESP_LOGI(
        TAG,
        "HTTP server stopped"
    );

    return ESP_OK;
}


bool web_server_is_running(void)
{
    return
        s_server !=
        NULL;
}
