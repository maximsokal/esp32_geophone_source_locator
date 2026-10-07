#include "web_server_internal.h"


esp_err_t web_webui_prepare(void)
{
    return ESP_OK;
}


esp_err_t web_webui_send_index(
    httpd_req_t *request
)
{
    return request == NULL
        ? ESP_ERR_INVALID_ARG
        : ESP_ERR_NOT_FOUND;
}


esp_err_t web_webui_register(
    httpd_handle_t server
)
{
    return server == NULL
        ? ESP_ERR_INVALID_ARG
        : ESP_ERR_NOT_SUPPORTED;
}
