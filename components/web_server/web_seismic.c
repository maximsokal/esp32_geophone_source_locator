#include "web_seismic.h"

#include "web_seismic_api.h"
#include "web_seismic_page.h"


static esp_err_t web_seismic_page_handler(
    httpd_req_t *request
)
{
    return
        web_seismic_page_send(
            request
        );
}


esp_err_t web_seismic_register(
    httpd_handle_t server
)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const httpd_uri_t page_uri = {
        .uri = "/seismic",
        .method = HTTP_GET,
        .handler = web_seismic_page_handler,
        .user_ctx = NULL,
    };

    esp_err_t err =
        httpd_register_uri_handler(
            server,
            &page_uri
        );

    if (err != ESP_OK) {
        return err;
    }

    return
        web_seismic_api_register(
            server
        );
}
