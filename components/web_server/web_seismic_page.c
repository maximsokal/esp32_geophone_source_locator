#include "web_seismic_page.h"

#include <stddef.h>

#include "web_static_assets.h"


esp_err_t web_seismic_page_send(
    httpd_req_t *request
)
{
    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *html =
        web_static_asset_data(
            WEB_STATIC_ASSET_SEISMIC_HTML
        );

    if (html == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    httpd_resp_set_type(
        request,
        "text/html; charset=utf-8"
    );

    httpd_resp_set_hdr(
        request,
        "Cache-Control",
        "no-store"
    );

    return
        httpd_resp_send(
            request,
            html,
            HTTPD_RESP_USE_STRLEN
        );
}
