#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esp_http_server.h"
#include "web_route_capacity.h"

static web_route_tracker_t s_registered;

esp_err_t web_server_route_tracking_register(const char *uri, int method)
{
    return web_route_tracker_register(
        &s_registered,
        (web_route_identity_t){ uri, method == HTTP_POST ? WEB_ROUTE_POST : WEB_ROUTE_GET }
    ) ? ESP_OK : ESP_FAIL;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *uri)
{
    return server != NULL && uri != NULL ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type) { (void)request; (void)type; return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *field, const char *value) { (void)request; (void)field; (void)value; return ESP_OK; }
esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status) { (void)request; (void)status; return ESP_OK; }
esp_err_t httpd_resp_send(httpd_req_t *request, const char *buffer, ssize_t length) { (void)request; (void)buffer; (void)length; return ESP_OK; }
esp_err_t httpd_resp_send_chunk(httpd_req_t *request, const char *buffer, ssize_t length) { (void)request; (void)buffer; (void)length; return ESP_OK; }
esp_err_t httpd_resp_send_err(httpd_req_t *request, int status, const char *message) { (void)request; (void)status; (void)message; return ESP_OK; }
bool wifi_manager_is_setup_mode(void) { return true; }
esp_err_t wifi_manager_save_sta_credentials(const char *ssid, const char *password) { (void)ssid; (void)password; return ESP_OK; }
esp_err_t wifi_manager_connect_saved(void) { return ESP_OK; }
esp_err_t app_event_bus_publish(int event, int value) { (void)event; (void)value; return ESP_OK; }
void vTaskDelay(unsigned ticks) { (void)ticks; }
bool web_https_onboarding_ca_install_confirmed(void) { return true; }
esp_err_t web_wifi_setup_page_send_form(httpd_req_t *request) { (void)request; return ESP_OK; }
esp_err_t web_wifi_setup_page_send_saved(httpd_req_t *request) { (void)request; return ESP_OK; }
esp_err_t web_wifi_setup_page_open_handler(httpd_req_t *request) { (void)request; return ESP_OK; }
esp_err_t web_wifi_setup_request_read_body(httpd_req_t *request, char *body, size_t size) { (void)request; (void)body; (void)size; return ESP_FAIL; }
esp_err_t web_wifi_setup_form_parse(char *body, char *ssid, size_t ssid_size, char *password, size_t password_size) { (void)body; (void)ssid; (void)ssid_size; (void)password; (void)password_size; return ESP_FAIL; }

#include "../../components/web_server/web_wifi_setup.c"

int main(void)
{
    assert(web_route_tracker_init(&s_registered, 2U));
    assert(web_wifi_setup_register((httpd_handle_t)1) == ESP_OK);

    size_t count = 0U;
    const web_route_identity_t *catalog = web_http_route_catalog(&count);
    assert(s_registered.count == 2U);
    for (size_t i = 0U; i < s_registered.count; ++i) {
        bool found = false;
        for (size_t j = 0U; j < count; ++j) {
            if (s_registered.active[i].method == catalog[j].method &&
                strcmp(s_registered.active[i].uri, catalog[j].uri) == 0) {
                found = true;
                break;
            }
        }
        assert(found);
    }
    return 0;
}
