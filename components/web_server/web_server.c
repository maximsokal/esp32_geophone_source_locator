#include "web_server.h"

#include "web_api_status.h"
#include "web_page.h"
#include "web_seismic.h"

#include "esp_http_server.h"
#include "esp_log.h"


static const char *TAG =
    "WEB_SERVER";


static httpd_handle_t s_server =
    NULL;


static esp_err_t web_server_index_handler(
    httpd_req_t *request
)
{
    const char *html =
        web_page_get_index_html();


    httpd_resp_set_type(
        request,
        "text/html; charset=utf-8"
    );


    httpd_resp_set_hdr(
        request,
        "Cache-Control",
        "no-store"
    );


    return httpd_resp_send(
        request,
        html,
        HTTPD_RESP_USE_STRLEN
    );
}


static esp_err_t web_server_health_handler(
    httpd_req_t *request
)
{
    static const char response[] =
        "OK";


    httpd_resp_set_type(
        request,
        "text/plain"
    );


    return httpd_resp_send(
        request,
        response,
        HTTPD_RESP_USE_STRLEN
    );
}


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
        8;

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


    const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = web_server_index_handler,
        .user_ctx = NULL,
    };


    err =
        httpd_register_uri_handler(
            s_server,
            &index_uri
        );


    if (err != ESP_OK) {

        web_server_stop();

        return err;
    }


    const httpd_uri_t health_uri = {
        .uri = "/health",
        .method = HTTP_GET,
        .handler = web_server_health_handler,
        .user_ctx = NULL,
    };


    err =
        httpd_register_uri_handler(
            s_server,
            &health_uri
        );


    if (err != ESP_OK) {

        web_server_stop();

        return err;
    }


    err =
        web_api_status_register(
            s_server
        );


    if (err != ESP_OK) {

        web_server_stop();

        return err;
    }


    err =
        web_seismic_register(
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


    ESP_LOGI(
        TAG,
        "Open http://192.168.4.1"
    );


    ESP_LOGI(
        TAG,
        "Seismic monitor: http://192.168.4.1/seismic"
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


    return httpd_register_err_handler(
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


    return httpd_register_err_handler(
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
    return s_server != NULL;
}