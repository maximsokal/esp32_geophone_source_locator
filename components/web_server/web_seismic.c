#include "web_seismic.h"

#include "web_seismic_api.h"
#include "web_seismic_page.h"


static esp_err_t web_seismic_page_handler(
    httpd_req_t *request
)
{
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
        web_seismic_page_get_html(),
        HTTPD_RESP_USE_STRLEN
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


    return web_seismic_api_register(
        server
    );
}