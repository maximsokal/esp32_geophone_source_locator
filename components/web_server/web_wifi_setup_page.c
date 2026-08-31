#include "web_server_internal.h"

#include "app_config.h"
#include "app_event_bus.h"
#include "wifi_manager.h"

#include "esp_log.h"


static const char *TAG =
    "WEB_WIFI_SETUP";


static const char s_setup_page[] =
    "<!doctype html>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>GeoLocator Wi-Fi Setup</title>"
    "<h2>GeoLocator Wi-Fi Setup</h2>"
    "<p>Enter this phone's hotspot name and password.</p>"
    "<form method=post action='/api/wifi/setup'>"
    "<p><label>Hotspot name<br>"
    "<input name=ssid maxlength=32 required></label></p>"
    "<p><label>Hotspot password<br>"
    "<input name=password type=password maxlength=63></label></p>"
    "<button type=submit>Save hotspot</button>"
    "</form>";


static const char s_saved_page[] =
    "<!doctype html>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>GeoLocator Wi-Fi Setup</title>"
    "<h2>Saved</h2>"
    "<p>Hotspot settings were saved.</p>"
    "<p>GeoLocator will leave GEOLOCATOR_SETUP and switch to station mode automatically.</p>"
    "<p>When this page disconnects, enable this phone's hotspot and keep its name/password unchanged.</p>"
    "<p>After GeoLocator connects, scan the HTTPS verification QR shown on its display.</p>";


static esp_err_t web_wifi_setup_page_send(
    httpd_req_t *request,
    const char *page
)
{
    if (
        request == NULL ||
        page == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
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
            page,
            HTTPD_RESP_USE_STRLEN
        );
}


esp_err_t web_wifi_setup_page_send_form(
    httpd_req_t *request
)
{
    return web_wifi_setup_page_send(
        request,
        s_setup_page
    );
}


esp_err_t web_wifi_setup_page_send_saved(
    httpd_req_t *request
)
{
    return web_wifi_setup_page_send(
        request,
        s_saved_page
    );
}


esp_err_t web_wifi_setup_page_open_handler(
    httpd_req_t *request
)
{
    if (
        !wifi_manager_is_setup_mode()
    ) {
        static const char response[] =
            "GeoLocator secure web UI is available over HTTPS.";

        httpd_resp_set_status(
            request,
            "302 Found"
        );

        httpd_resp_set_hdr(
            request,
            "Location",
            APP_MDNS_WEB_URL
        );

        httpd_resp_set_type(
            request,
            "text/plain; charset=utf-8"
        );

        return
            httpd_resp_send(
                request,
                response,
                HTTPD_RESP_USE_STRLEN
            );
    }

    const bool ca_confirmed =
        web_https_onboarding_ca_install_confirmed();

    esp_err_t err =
        web_wifi_setup_send_page(
            request
        );

    if (err != ESP_OK) {
        return err;
    }

    if (!ca_confirmed) {
        ESP_LOGI(
            TAG,
            "Setup page redirected to Root CA onboarding"
        );

        return ESP_OK;
    }

    ESP_LOGI(
        TAG,
        "Setup page opened from wizard URL"
    );

    (void)app_event_bus_publish(
        APP_EVENT_WIFI_SETUP_PAGE_OPENED,
        0
    );

    return ESP_OK;
}
