#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "storage_manager.h"
#include "web_localization_storage.h"

static char s_status[64];
static char s_type[96];
static char s_cache_control[32];
static char s_body[96];
static unsigned s_send_calls;

esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type)
{
    assert(request != NULL);
    snprintf(s_type, sizeof(s_type), "%s", type);
    return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *field, const char *value)
{
    assert(request != NULL);
    if (strcmp(field, "Cache-Control") == 0) {
        snprintf(s_cache_control, sizeof(s_cache_control), "%s", value);
    }
    return ESP_OK;
}

esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status)
{
    assert(request != NULL);
    snprintf(s_status, sizeof(s_status), "%s", status);
    return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t *request, const char *body, ssize_t length)
{
    assert(request != NULL && body != NULL);
    assert(length == HTTPD_RESP_USE_STRLEN);
    snprintf(s_body, sizeof(s_body), "%s", body);
    ++s_send_calls;
    return ESP_OK;
}

int main(void)
{
    httpd_req_t request = { .query = "object_id=OBJ_0001&session_id=S000001" };
    esp_err_t response = ESP_FAIL;
    storage_manager_test_set_ready(false);
    assert(web_localization_runs_reject_if_storage_unavailable(&request, &response));
    assert(response == ESP_OK);
    assert(strcmp(s_status, "503 Service Unavailable") == 0);
    assert(strcmp(s_type, "application/json; charset=utf-8") == 0);
    assert(strcmp(s_cache_control, "no-store") == 0);
    assert(strcmp(s_body, "{\"storage_unavailable\":true}") == 0);
    assert(s_send_calls == 1U);

    storage_manager_test_set_ready(true);
    assert(!web_localization_runs_reject_if_storage_unavailable(&request, &response));
    assert(s_send_calls == 1U);
    puts("web_localization_storage_test: PASS");
    return 0;
}
