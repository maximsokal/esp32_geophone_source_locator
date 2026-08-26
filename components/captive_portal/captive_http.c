#include "captive_http.h"

#include "esp_http_server.h"
#include "esp_log.h"


static const char *TAG =
    "CAPTIVE_HTTP";


static const char s_captive_response[] =
    "<!doctype html>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<meta http-equiv=refresh content='0;url=http://192.168.4.1/'>"
    "<title>GeoLocator Wi-Fi Setup</title>"
    "<p>Opening GeoLocator Wi-Fi Setup...</p>"
    "<p><a href='http://192.168.4.1/'>Open setup</a></p>";


esp_err_t captive_http_not_found_handler(
    httpd_req_t *request,
    httpd_err_code_t error
)
{
    (void)error;

    /*
     * Android captive portal probes expect HTTP 204 on a network with
     * unrestricted Internet access. Returning HTTP 200 with content is
     * a strong captive-portal signal and is more reliable on Samsung
     * devices than redirect-only responses.
     *
     * The meta refresh then moves the captive browser to the local
     * GeoLocator setup form.
     */
    httpd_resp_set_status(
        request,
        "200 OK"
    );

    httpd_resp_set_hdr(
        request,
        "Cache-Control",
        "no-store"
    );

    httpd_resp_set_type(
        request,
        "text/html; charset=utf-8"
    );

    ESP_LOGD(
        TAG,
        "Serving captive response for %s",
        request->uri
    );

    return
        httpd_resp_send(
            request,
            s_captive_response,
            HTTPD_RESP_USE_STRLEN
        );
}
