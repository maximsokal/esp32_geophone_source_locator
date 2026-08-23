#include "captive_http.h"

#include "esp_http_server.h"
#include "esp_log.h"


static const char *TAG =
    "CAPTIVE_HTTP";


esp_err_t captive_http_not_found_handler(
    httpd_req_t *request,
    httpd_err_code_t error
)
{
    (void)error;


    /*
     * Captive portal detection requests use many different
     * paths and host names.
     *
     * Redirect every unknown HTTP path back to the local
     * GeoLocator root page.
     */
    httpd_resp_set_status(
        request,
        "303 See Other"
    );


    httpd_resp_set_hdr(
        request,
        "Location",
        "/"
    );


    httpd_resp_set_hdr(
        request,
        "Cache-Control",
        "no-store"
    );


    httpd_resp_set_type(
        request,
        "text/plain"
    );


    static const char response[] =
        "Redirect to GeoLocator captive portal";


    ESP_LOGD(
        TAG,
        "Redirecting %s to captive portal",
        request->uri
    );


    /*
     * A response body is intentional.
     *
     * Some captive portal detection implementations,
     * including iOS behavior described by Espressif,
     * require response content instead of an empty redirect.
     */
    return httpd_resp_send(
        request,
        response,
        HTTPD_RESP_USE_STRLEN
    );
}