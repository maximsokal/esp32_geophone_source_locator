#include "web_localization_storage.h"

#if defined(ESP_PLATFORM)
#include "storage_manager.h"
#else
#include <stdbool.h>
bool storage_manager_is_ready(void);
#endif

esp_err_t web_localization_runs_send_storage_unavailable(httpd_req_t *request)
{
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_status(request, "503 Service Unavailable");
    return httpd_resp_send(
        request,
        "{\"storage_unavailable\":true}",
        HTTPD_RESP_USE_STRLEN
    );
}

bool web_localization_runs_reject_if_storage_unavailable(
    httpd_req_t *request,
    esp_err_t *response_out
)
{
    if (response_out == NULL || storage_manager_is_ready()) return false;
    *response_out = web_localization_runs_send_storage_unavailable(request);
    return true;
}
