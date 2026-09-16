#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "web_route_capacity.h"

static web_route_tracker_t s_registered;
static esp_err_t (*s_wifi_post_handler)(httpd_req_t *request);
static bool s_shutdown_requested;
static bool s_shutdown_after_read;
static bool s_shutdown_during_delay;
static unsigned s_read_calls;
static unsigned s_parse_calls;
static unsigned s_save_calls;
static unsigned s_saved_page_calls;
static unsigned s_connect_calls;
static unsigned s_delay_calls;
static unsigned s_app_event_posts;
static char s_last_status[32];

static void reset_behavior(void)
{
    s_shutdown_requested = false;
    s_shutdown_after_read = false;
    s_shutdown_during_delay = false;
    s_read_calls = 0U;
    s_parse_calls = 0U;
    s_save_calls = 0U;
    s_saved_page_calls = 0U;
    s_connect_calls = 0U;
    s_delay_calls = 0U;
    s_app_event_posts = 0U;
    s_last_status[0] = '\0';
}

esp_err_t web_server_route_tracking_register(const char *uri, int method)
{
    return web_route_tracker_register(
        &s_registered,
        (web_route_identity_t){ uri, method == HTTP_POST ? WEB_ROUTE_POST : WEB_ROUTE_GET }
    ) ? ESP_OK : ESP_FAIL;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *uri)
{
    assert(server != NULL);
    assert(uri != NULL);
    if (uri->method == HTTP_POST && strcmp(uri->uri, "/api/wifi/setup") == 0) {
        s_wifi_post_handler = uri->handler;
    }
    return ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type)
{
    (void)request;
    (void)type;
    return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *field, const char *value)
{
    (void)request;
    (void)field;
    (void)value;
    return ESP_OK;
}

esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status)
{
    (void)request;
    snprintf(s_last_status, sizeof(s_last_status), "%s", status);
    return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t *request, const char *buffer, ssize_t length)
{
    (void)request;
    (void)buffer;
    (void)length;
    return ESP_OK;
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *request, const char *buffer, ssize_t length)
{
    (void)request;
    (void)buffer;
    (void)length;
    return ESP_OK;
}

esp_err_t httpd_resp_send_err(httpd_req_t *request, int status, const char *message)
{
    (void)request;
    (void)status;
    (void)message;
    return ESP_OK;
}

bool power_control_is_shutdown_requested(void)
{
    return s_shutdown_requested;
}

bool wifi_manager_is_setup_mode(void)
{
    return true;
}

esp_err_t wifi_manager_save_sta_credentials(const char *ssid, const char *password)
{
    assert(strcmp(ssid, "test-ssid") == 0);
    assert(strcmp(password, "test-password") == 0);
    ++s_save_calls;
    return ESP_OK;
}

esp_err_t wifi_manager_connect_saved(void)
{
    ++s_connect_calls;
    return ESP_OK;
}

esp_err_t app_events_post(int event, int value)
{
    (void)event;
    (void)value;
    ++s_app_event_posts;
    return ESP_OK;
}

void vTaskDelay(unsigned ticks)
{
    (void)ticks;
    ++s_delay_calls;
    if (s_shutdown_during_delay) {
        s_shutdown_requested = true;
    }
}

bool web_https_onboarding_ca_install_confirmed(void)
{
    return true;
}

esp_err_t web_wifi_setup_page_send_form(httpd_req_t *request)
{
    (void)request;
    return ESP_OK;
}

esp_err_t web_wifi_setup_page_send_saved(httpd_req_t *request)
{
    (void)request;
    ++s_saved_page_calls;
    return ESP_OK;
}

esp_err_t web_wifi_setup_page_open_handler(httpd_req_t *request)
{
    (void)request;
    return ESP_OK;
}

esp_err_t web_wifi_setup_request_read_body(httpd_req_t *request, char *body, size_t size)
{
    (void)request;
    assert(body != NULL);
    assert(size > 1U);
    ++s_read_calls;
    snprintf(body, size, "ssid=x&password=y");
    if (s_shutdown_after_read) {
        s_shutdown_requested = true;
    }
    return ESP_OK;
}

esp_err_t web_wifi_setup_form_parse(
    char *body,
    char *ssid,
    size_t ssid_size,
    char *password,
    size_t password_size
)
{
    assert(body != NULL);
    ++s_parse_calls;
    snprintf(ssid, ssid_size, "test-ssid");
    snprintf(password, password_size, "test-password");
    return ESP_OK;
}

#include "../../components/web_server/web_wifi_setup.c"

int main(void)
{
    assert(web_route_tracker_init(&s_registered, 2U));
    assert(web_wifi_setup_register((httpd_handle_t)1) == ESP_OK);
    assert(s_wifi_post_handler != NULL);

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

    httpd_req_t request = {0};

    reset_behavior();
    s_shutdown_requested = true;
    assert(s_wifi_post_handler(&request) == ESP_OK);
    assert(strcmp(s_last_status, "409 Conflict") == 0);
    assert(s_read_calls == 0U);
    assert(s_save_calls == 0U);
    assert(s_connect_calls == 0U);

    reset_behavior();
    assert(s_wifi_post_handler(&request) == ESP_OK);
    assert(s_read_calls == 1U);
    assert(s_parse_calls == 1U);
    assert(s_save_calls == 1U);
    assert(s_saved_page_calls == 1U);
    assert(s_app_event_posts == 1U);
    assert(s_delay_calls == 1U);
    assert(s_connect_calls == 1U);

    reset_behavior();
    s_shutdown_after_read = true;
    assert(s_wifi_post_handler(&request) == ESP_OK);
    assert(strcmp(s_last_status, "409 Conflict") == 0);
    assert(s_read_calls == 1U);
    assert(s_parse_calls == 1U);
    assert(s_save_calls == 0U);
    assert(s_delay_calls == 0U);
    assert(s_connect_calls == 0U);

    reset_behavior();
    s_shutdown_during_delay = true;
    assert(s_wifi_post_handler(&request) == ESP_OK);
    assert(s_save_calls == 1U);
    assert(s_saved_page_calls == 1U);
    assert(s_delay_calls == 1U);
    assert(s_connect_calls == 0U);

    puts("web Wi-Fi shutdown admission tests passed");
    return 0;
}
