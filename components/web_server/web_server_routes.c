#include "web_server_internal.h"

#include "esp_log.h"

#include <stddef.h>

#include "app_config.h"
#include "wifi_manager.h"
#include "web_client_log.h"

static const char *TAG = "WEB_ROUTES";

static esp_err_t web_server_bootstrap_index_handler(httpd_req_t *request)
{
    if (wifi_manager_is_setup_mode()) {
        return web_wifi_setup_send_page(request);
    }

    static const char response[] = "GeoLocator secure web UI is available over HTTPS.";
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", APP_MDNS_WEB_URL);
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    return httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t web_server_secure_index_handler(httpd_req_t *request)
{
    ESP_LOGI(TAG, "HTTPS GET / entered");
    const esp_err_t webui_err = web_webui_flash_send_index(request);
    if (webui_err == ESP_OK) return ESP_OK;
    if (webui_err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "React flash root response failed: %s", esp_err_to_name(webui_err));
        return webui_err;
    }

    ESP_LOGW(TAG, "React flash root unavailable; serving legacy embedded dashboard");
    return web_page_send_index(request);
}

static esp_err_t web_server_health_handler(httpd_req_t *request)
{
    static const char response[] = "OK";
    httpd_resp_set_type(request, "text/plain");
    return httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t web_server_register_root_and_health(
    httpd_handle_t server,
    esp_err_t (*root_handler)(httpd_req_t *request)
)
{
    const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
        .user_ctx = NULL,
    };
    esp_err_t err = httpd_register_uri_handler(server, &index_uri);
    if (err != ESP_OK) return err;

    const httpd_uri_t health_uri = {
        .uri = "/health",
        .method = HTTP_GET,
        .handler = web_server_health_handler,
        .user_ctx = NULL,
    };
    return httpd_register_uri_handler(server, &health_uri);
}

esp_err_t web_server_bootstrap_routes_register(httpd_handle_t server)
{
    if (server == NULL) return ESP_ERR_INVALID_ARG;

    esp_err_t err = web_server_register_root_and_health(server, web_server_bootstrap_index_handler);
    if (err != ESP_OK) return err;
    err = web_wifi_setup_register(server);
    if (err != ESP_OK) return err;
    err = web_https_onboarding_bootstrap_register(server);
    if (err != ESP_OK) return err;

    /* Reuse the two bootstrap slots instead of increasing server RAM. */
    err = httpd_unregister_uri_handler(server, "/setup/ca.crt", HTTP_GET);
    if (err != ESP_OK) return err;
    err = httpd_unregister_uri_handler(server, "/health", HTTP_GET);
    if (err != ESP_OK) return err;
    return web_https_onboarding_ui_bootstrap_register(server);
}

esp_err_t web_server_routes_register(httpd_handle_t server)
{
    if (server == NULL) return ESP_ERR_INVALID_ARG;

    esp_err_t err = web_server_register_root_and_health(server, web_server_secure_index_handler);
    if (err != ESP_OK) return err;

    /* HTTPS remains inside the fixed handler budget; browser diagnostics win the health slot. */
    err = httpd_unregister_uri_handler(server, "/health", HTTP_GET);
    if (err != ESP_OK) return err;

    err = web_client_log_register(server);
    if (err != ESP_OK) return err;

    /*
     * One wildcard owns both React chunks and the four legacy CSS/JS assets.
     * This replaces four exact static-asset handlers and leaves room for the
     * single typed device-control endpoint without growing httpd tables/RAM.
     */
    err = web_webui_flash_register(server);
    if (err != ESP_OK) return err;

    err = web_api_status_register(server);
    if (err != ESP_OK) return err;

    err = web_api_control_register(server);
    if (err != ESP_OK) return err;

    err = web_seismic_register(server);
    if (err != ESP_OK) return err;

    err = web_https_onboarding_secure_register(server);
    if (err != ESP_OK) return err;

    return web_https_onboarding_ui_secure_register(server);
}
