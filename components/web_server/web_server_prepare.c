#include "web_server.h"

#include "esp_err.h"
#include "esp_log.h"

#include "web_server_internal.h"


static const char *TAG =
    "WEB_PREPARE";


esp_err_t web_server_prepare_webui(void)
{
#if WEB_SERVER_WEBUI_SEED_EMBEDDED
    /*
     * The production React routes are served directly from the firmware image
     * by web_webui_flash_register()/web_webui_flash_send_index(). Mirroring the
     * exact same bundle to SD before Wi-Fi starts adds seconds of boot time,
     * extra FAT traffic and card wear without participating in runtime serving.
     */
    ESP_LOGI(
        TAG,
        "React Web UI is flash-authoritative; SD mirror seed skipped"
    );

    return ESP_OK;
#else
    /*
     * Keep the legacy preparation path available for unusual builds that do
     * not embed the WebUI seed. Normal firmware builds fail closed in CMake if
     * the generated embedded bundle is incomplete.
     */
    ESP_LOGW(
        TAG,
        "Embedded React Web UI unavailable; using legacy SD preparation"
    );

    return web_webui_prepare();
#endif
}
