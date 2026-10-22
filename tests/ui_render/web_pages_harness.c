#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "web_server_internal.h"
#include "app_events.h"

static httpd_uri_t routes[12];
static size_t route_count;
static char response[131072];
static size_t response_length;
static int status_code;
static const char *content_type;
static const char *cache_control;
static const char *location;
static bool sent, setup_mode = true, shutdown_requested;
static const char *active_query = "";
static bool save_failure, send_failure;
static unsigned saves, connections, events;
static char *index_html, *seismic_html;

size_t strlcpy(char *destination, const char *source, size_t size)
{
    size_t length = strlen(source);
    if (size > 0) {
        size_t copied = length < size - 1 ? length : size - 1;
        memcpy(destination, source, copied);
        destination[copied] = '\0';
    }
    return length;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *route)
{
    assert(server && route && route_count < sizeof(routes) / sizeof(routes[0]));
    routes[route_count++] = *route;
    return ESP_OK;
}
esp_err_t web_server_route_tracking_register(const char *uri, int method)
{
    assert(uri && (method == HTTP_GET || method == HTTP_POST));
    return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type)
{
    assert(request && type); content_type = type; return ESP_OK;
}
esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *field, const char *value)
{
    assert(request && field && value);
    if (strcmp(field, "Cache-Control") == 0) cache_control = value;
    else if (strcmp(field, "Location") == 0) location = value;
    else assert(!"Unexpected response header");
    return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status)
{
    assert(request && status); status_code = atoi(status); return ESP_OK;
}
esp_err_t httpd_resp_send_chunk(httpd_req_t *request, const char *data, ssize_t length)
{
    assert(request && !sent && length >= 0);
    if (send_failure) return ESP_FAIL;
    if (!data) { assert(length == 0); sent = true; return ESP_OK; }
    assert(response_length + (size_t)length < sizeof(response));
    memcpy(response + response_length, data, (size_t)length);
    response_length += (size_t)length;
    response[response_length] = '\0';
    return ESP_OK;
}
esp_err_t httpd_resp_send(httpd_req_t *request, const char *data, ssize_t length)
{
    assert(data);
    esp_err_t result = httpd_resp_send_chunk(request, data,
        length == HTTPD_RESP_USE_STRLEN ? (ssize_t)strlen(data) : length);
    if (result == ESP_OK) sent = true;
    return result;
}
esp_err_t httpd_resp_send_err(httpd_req_t *request, int status, const char *message)
{
    /* SDK boundary: retain the production error message as plain text. */
    status_code = status;
    content_type = "text/plain; charset=utf-8";
    return httpd_resp_send(request, message, HTTPD_RESP_USE_STRLEN);
}
int httpd_req_recv(httpd_req_t *request, char *data, size_t length)
{
    assert(request && data && request->received + length <= (size_t)request->content_len);
    memcpy(data, request->body + request->received, length);
    request->received += length;
    return (int)length;
}
int httpd_req_to_sockfd(httpd_req_t *request) { assert(request); return -1; }
int setsockopt(int fd, int level, int option, const void *value, size_t size)
{ (void)fd; (void)level; (void)option; (void)value; (void)size; assert(!"No real sockets"); return -1; }
bool wifi_manager_is_setup_mode(void) { return setup_mode; }
uint32_t esp_random(void) { return 0x1234; }
bool power_control_is_shutdown_requested(void) { return shutdown_requested; }
esp_err_t wifi_manager_save_sta_credentials(const char *ssid, const char *password)
{
    assert(strcmp(ssid, "Field Phone") == 0 && strcmp(password, "field+password") == 0);
    ++saves; return save_failure ? ESP_FAIL : ESP_OK;
}
esp_err_t wifi_manager_connect_saved(void) { ++connections; return ESP_OK; }
esp_err_t app_events_post(app_event_type_t event, int32_t value) { assert(event > 0 && value >= 0); ++events; return ESP_OK; }
void vTaskDelay(unsigned ticks) { assert(ticks == 750); }
const char *web_static_asset_data(web_static_asset_id_t asset)
{
    if (asset == WEB_STATIC_ASSET_INDEX_HTML) return index_html;
    assert(asset == WEB_STATIC_ASSET_SEISMIC_HTML); return seismic_html;
}

size_t httpd_req_get_url_query_len(httpd_req_t *request) { return strlen(request->query); }
esp_err_t httpd_req_get_url_query_str(httpd_req_t *request, char *query, size_t size)
{ return strlcpy(query, request->query, size) < size ? ESP_OK : ESP_ERR_INVALID_SIZE; }
esp_err_t httpd_query_key_value(const char *query, const char *key, char *value, size_t size)
{
    size_t length = strlen(key);
    while (query && *query) {
        const char *next = strchr(query, '&');
        if (strncmp(query, key, length) == 0 && query[length] == '=') {
            query += length + 1;
            size_t count = next ? (size_t)(next - query) : strlen(query);
            if (count >= size) return ESP_ERR_INVALID_SIZE;
            memcpy(value, query, count); value[count] = '\0'; return ESP_OK;
        }
        query = next ? next + 1 : NULL;
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t (*handler(const char *uri))(httpd_req_t *)
{
    for (size_t i = 0; i < route_count; ++i) if (strcmp(routes[i].uri, uri) == 0) return routes[i].handler;
    assert(!"Missing production route"); return NULL;
}
static char *read_asset(const char *directory, const char *name)
{
    char path[2048];
    assert(snprintf(path, sizeof(path), "%s/%s", directory, name) > 0);
    FILE *file = fopen(path, "rb"); assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file); assert(length > 0);
    rewind(file);
    char *data = malloc((size_t)length + 1); assert(data);
    assert(fread(data, 1, (size_t)length, file) == (size_t)length);
    data[length] = '\0'; assert(fclose(file) == 0); return data;
}
static void reset_response(void)
{
    response[0] = '\0'; response_length = 0; sent = false;
    status_code = 200; content_type = ""; cache_control = ""; location = "";
}
static void capture(FILE *manifest, const char *directory, const char *name,
                    esp_err_t (*send)(httpd_req_t *), const char *body, int expected_status, esp_err_t expected_result)
{
    reset_response();
    httpd_req_t request = { .body = body, .content_len = strlen(body), .query = active_query };
    esp_err_t result = send(&request);
    assert(result == expected_result && sent && response_length > 0 && status_code == expected_status);
    char path[2048];
    assert(snprintf(path, sizeof(path), "%s/%s.html", directory, name) > 0);
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fwrite(response, 1, response_length, file) == response_length);
    assert(fclose(file) == 0);
    fprintf(manifest, "%s{\"id\":\"%s\",\"file\":\"%s.html\",\"status\":%d,\"content_type\":\"%s\","
        "\"cache_control\":\"%s\",\"location\":\"%s\",\"handler_result\":%d,\"bytes\":%zu}",
        ftell(manifest) > 10 ? ",\n" : "", name, name, status_code, content_type, cache_control, location, result, response_length);
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    index_html = read_asset(argv[1], "index.html");
    seismic_html = read_asset(argv[1], "seismic.html");
    assert(web_wifi_setup_register((void *)1) == ESP_OK);
    assert(web_https_onboarding_ui_bootstrap_register((void *)1) == ESP_OK);
    assert(web_https_onboarding_ui_secure_register((void *)1) == ESP_OK);
    assert(web_https_onboarding_bootstrap_register((void *)1) == ESP_OK);
    assert(web_https_onboarding_secure_register((void *)1) == ESP_OK);
    assert(route_count == 9);
    assert(web_wifi_setup_page_send_form(NULL) == ESP_ERR_INVALID_ARG);
    assert(web_wifi_setup_page_send_saved(NULL) == ESP_ERR_INVALID_ARG);
    assert(web_page_send_index(NULL) == ESP_ERR_INVALID_ARG);
    assert(web_seismic_page_send(NULL) == ESP_ERR_INVALID_ARG);
    httpd_req_t request = {0};
    send_failure = true; reset_response();
    assert(web_wifi_setup_page_send_form(&request) == ESP_FAIL && !sent);
    send_failure = false;
    char path[2048];
    assert(snprintf(path, sizeof(path), "%s/responses.json", argv[2]) > 0);
    FILE *manifest = fopen(path, "wb"); assert(manifest); fputs("{\"cases\":[", manifest);
    capture(manifest, argv[2], "ca-legacy", handler("/setup/ca"), "", 200, ESP_OK);
    capture(manifest, argv[2], "wifi-needs-ca", handler("/setup"), "", 302, ESP_OK);
    capture(manifest, argv[2], "wifi-ca-required", handler("/api/wifi/setup"), "", 403, ESP_ERR_INVALID_STATE);
    active_query = "installed=1";
    capture(manifest, argv[2], "ca-confirmed", handler("/setup/ca"), "", 200, ESP_OK);
    active_query = "";
    capture(manifest, argv[2], "wifi-form", handler("/setup"), "", 200, ESP_OK);
    setup_mode = false;
    capture(manifest, argv[2], "wifi-station-redirect", handler("/setup"), "", 302, ESP_OK);
    capture(manifest, argv[2], "wifi-not-setup", handler("/api/wifi/setup"), "", 403, ESP_ERR_INVALID_STATE);
    setup_mode = true; shutdown_requested = true;
    capture(manifest, argv[2], "wifi-shutdown", handler("/api/wifi/setup"), "", 409, ESP_OK);
    shutdown_requested = false;
    capture(manifest, argv[2], "wifi-invalid-body", handler("/api/wifi/setup"), "", 400, ESP_ERR_INVALID_SIZE);
    capture(manifest, argv[2], "wifi-invalid-form", handler("/api/wifi/setup"), "ssid=%GG", 400, ESP_ERR_INVALID_ARG);
    const char *valid = "ssid=Field+Phone&password=field%2Bpassword";
    save_failure = true;
    capture(manifest, argv[2], "wifi-save-error", handler("/api/wifi/setup"), valid, 500, ESP_FAIL);
    save_failure = false;
    capture(manifest, argv[2], "wifi-saved", handler("/api/wifi/setup"), valid, 200, ESP_OK);
    assert(saves == 2 && connections == 1 && events == 4);
    capture(manifest, argv[2], "ca-guide", handler("/setup/ca-guide"), "", 200, ESP_OK);
    capture(manifest, argv[2], "ca-unavailable", handler("/setup/ca-download"), "", 500, ESP_OK);
    capture(manifest, argv[2], "verify-ui", handler("/setup/verify-ui"), "", 200, ESP_OK);
    capture(manifest, argv[2], "ca-legacy-unavailable", handler("/setup/ca.crt"), "", 503, ESP_OK);
    capture(manifest, argv[2], "verify-expired", handler("/setup/verify"), "", 403, ESP_OK);
    capture(manifest, argv[2], "verify-report-expired", handler("/api/setup/verify"), "", 403, ESP_OK);
    active_query = "t=1234";
    capture(manifest, argv[2], "verify-legacy", handler("/setup/verify"), "", 200, ESP_OK);
    capture(manifest, argv[2], "verify-report-invalid", handler("/api/setup/verify"), "{", 400, ESP_OK);
    for (unsigned mask = 0; mask < 16; ++mask) {
        char name[32], report[256];
        snprintf(name, sizeof(name), "verify-report-%u", mask);
        snprintf(report, sizeof(report), "{\"secure_context\":%s,\"camera\":%s,\"xr_api\":%s,\"immersive_ar\":%s}",
            mask & 1 ? "true" : "false", mask & 2 ? "true" : "false", mask & 4 ? "true" : "false", mask & 8 ? "true" : "false");
        capture(manifest, argv[2], name, handler("/api/setup/verify"), report, 200, ESP_OK);
    }
    active_query = "";
    capture(manifest, argv[2], "fallback", web_page_send_index, "", 200, ESP_OK);
    assert(!strstr(response, "{{WIFI_SSID}}") && strstr(response, "GEOLOCATOR_SETUP"));
    capture(manifest, argv[2], "seismic", web_seismic_page_send, "", 200, ESP_OK);
    fputs("]}\n", manifest); assert(fclose(manifest) == 0);
    free(index_html); free(seismic_html);
    puts("WEB_PAGES_CAPTURE: PASS (38 responses; real builders, SDK/radio/JSON fakes)");
    return 0;
}
