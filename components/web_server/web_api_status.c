#include "web_api_status.h"

#include "web_status_json.h"

#include "esp_http_server.h"
#include "esp_log.h"


static const char *TAG =
    "WEB_API_STATUS";


static esp_err_t web_api_status_handler(
    httpd_req_t *request
)
{
    char *json = NULL;


    esp_err_t err =
        web_status_json_build(
            &json
        );


    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Failed to build status JSON: %s",
            esp_err_to_name(err)
        );


        return httpd_resp_send_err(
            request,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Failed to build device status"
        );
    }


    httpd_resp_set_type(
        request,
        "application/json; charset=utf-8"
    );


    httpd_resp_set_hdr(
        request,
        "Cache-Control",
        "no-store"
    );


    err =
        httpd_resp_send(
            request,
            json,
            HTTPD_RESP_USE_STRLEN
        );


    web_status_json_free(
        json
    );


    return err;
}


esp_err_t web_api_status_register(
    httpd_handle_t server
)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }


    const httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = web_api_status_handler,
        .user_ctx = NULL,
    };


    esp_err_t err =
        httpd_register_uri_handler(
            server,
            &status_uri
        );


    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Failed to register /api/status: %s",
            esp_err_to_name(err)
        );

        return err;
    }


    ESP_LOGI(
        TAG,
        "Registered GET /api/status"
    );


    return ESP_OK;
}