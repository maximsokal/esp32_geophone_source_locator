#include "insights_bridge.h"

#include <stdint.h>
#include <string.h>

#include "runtime_observability.h"

#include "esp_heap_caps.h"
#include "esp_log.h"


static const char *TAG = "INSIGHTS_BRIDGE";
static bool s_initialized = false;


#if defined(CONFIG_APP_ESP_INSIGHTS) && CONFIG_APP_ESP_INSIGHTS

#include "esp_diagnostics.h"
#include "esp_insights.h"


/*
 * HTTPS auth material is intentionally supplied by a separate, private
 * integration. No key is stored in this repository or sdkconfig.defaults.
 */
const char *app_esp_insights_auth_key(void) __attribute__((weak));


static bool insights_has_auth_key(void)
{
#if defined(CONFIG_ESP_INSIGHTS_TRANSPORT_HTTPS) && CONFIG_ESP_INSIGHTS_TRANSPORT_HTTPS
    if (app_esp_insights_auth_key == NULL) {
        return false;
    }

    const char *key = app_esp_insights_auth_key();
    return key != NULL && key[0] != '\0';
#else
    return true;
#endif
}


#if defined(CONFIG_DIAG_ENABLE_METRICS) && CONFIG_DIAG_ENABLE_METRICS
static void insights_register_metrics(void)
{
    (void)esp_diag_metrics_register(
        "runtime", "internal_free", "Internal free bytes", "runtime",
        ESP_DIAG_DATA_TYPE_UINT
    );
    (void)esp_diag_metrics_register(
        "runtime", "internal_largest", "Internal largest block", "runtime",
        ESP_DIAG_DATA_TYPE_UINT
    );
    (void)esp_diag_metrics_register(
        "runtime", "psram_free", "PSRAM free bytes", "runtime",
        ESP_DIAG_DATA_TYPE_UINT
    );
    (void)esp_diag_metrics_register(
        "runtime", "psram_largest", "PSRAM largest block", "runtime",
        ESP_DIAG_DATA_TYPE_UINT
    );
    (void)esp_diag_metrics_register(
        "runtime", "acquisition_heartbeat", "Acquisition heartbeat", "runtime",
        ESP_DIAG_DATA_TYPE_UINT
    );
    (void)esp_diag_metrics_register(
        "runtime", "recorder_heartbeat", "Recorder heartbeat", "runtime",
        ESP_DIAG_DATA_TYPE_UINT
    );
    (void)esp_diag_metrics_register(
        "runtime", "ui_heartbeat", "UI heartbeat", "runtime",
        ESP_DIAG_DATA_TYPE_UINT
    );
}
#endif

#endif /* optional ESP Insights declarations */


esp_err_t insights_bridge_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

#if defined(CONFIG_APP_ESP_INSIGHTS) && CONFIG_APP_ESP_INSIGHTS
    if (!insights_has_auth_key()) {
        ESP_LOGW(
            TAG,
            "ESP Insights requested but no private auth-key provider is installed; bridge remains inactive"
        );
        return ESP_OK;
    }

    esp_insights_config_t config = {
        /* Metrics only: application logs may contain device-specific data. */
        .log_type = 0U,
    };

#if defined(CONFIG_ESP_INSIGHTS_TRANSPORT_HTTPS) && CONFIG_ESP_INSIGHTS_TRANSPORT_HTTPS
    config.auth_key = app_esp_insights_auth_key();
#endif

    /* The agent is additive and its failure must never block device boot. */
    const esp_err_t init_err = esp_insights_init(&config);
    if (init_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "ESP Insights initialization unavailable; continuing without it: %s",
            esp_err_to_name(init_err)
        );
        return ESP_OK;
    }

#if defined(CONFIG_DIAG_ENABLE_METRICS) && CONFIG_DIAG_ENABLE_METRICS
    insights_register_metrics();
#endif

    s_initialized = true;
    ESP_LOGI(TAG, "ESP Insights bridge enabled with selected runtime metrics only");
    return ESP_OK;
#else
    ESP_LOGI(TAG, "ESP Insights bridge disabled (CONFIG_APP_ESP_INSIGHTS=n)");
    return ESP_OK;
#endif
}


esp_err_t insights_bridge_publish_runtime(void)
{
#if defined(CONFIG_APP_ESP_INSIGHTS) && CONFIG_APP_ESP_INSIGHTS && \
    defined(CONFIG_DIAG_ENABLE_METRICS) && CONFIG_DIAG_ENABLE_METRICS
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    runtime_observability_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    runtime_observability_get_snapshot(&snapshot);

    (void)esp_diag_metrics_add_uint(
        "internal_free",
        (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
    );
    (void)esp_diag_metrics_add_uint(
        "internal_largest",
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
    );
    (void)esp_diag_metrics_add_uint(
        "psram_free",
        (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
    );
    (void)esp_diag_metrics_add_uint(
        "psram_largest",
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)
    );
    (void)esp_diag_metrics_add_uint(
        "acquisition_heartbeat",
        (uint32_t)snapshot.tasks[RUNTIME_TASK_ACQUISITION].heartbeat
    );
    (void)esp_diag_metrics_add_uint(
        "recorder_heartbeat",
        (uint32_t)snapshot.tasks[RUNTIME_TASK_RECORDER].heartbeat
    );
    (void)esp_diag_metrics_add_uint(
        "ui_heartbeat",
        (uint32_t)snapshot.tasks[RUNTIME_TASK_UI].heartbeat
    );
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}


bool insights_bridge_enabled(void)
{
    return s_initialized;
}
