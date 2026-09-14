#include "seismic_recorder_storage_preflight.h"

#include <limits.h>
#include <stddef.h>

#include "esp_log.h"

#include "sd_card_info.h"
#include "seismic_buffer.h"
#include "storage_manager.h"

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#ifndef CONFIG_SEISMIC_RECORDER_PSRAM_CACHE_KB
#define CONFIG_SEISMIC_RECORDER_PSRAM_CACHE_KB 1024
#endif


static const char *TAG = "SEISMIC_RECORDER_PREFLIGHT";


static uint64_t seismic_recorder_saturating_multiply(
    uint64_t left,
    uint64_t right
)
{
    if (left == 0U || right == 0U) {
        return 0U;
    }

    if (left > UINT64_MAX / right) {
        return UINT64_MAX;
    }

    return left * right;
}


static uint64_t seismic_recorder_saturating_add(
    uint64_t left,
    uint64_t right
)
{
    if (left > UINT64_MAX - right) {
        return UINT64_MAX;
    }

    return left + right;
}


uint64_t seismic_recorder_storage_required_bytes(
    uint32_t sample_rate_hz,
    size_t frame_size_bytes,
    uint64_t cache_reserve_bytes
)
{
    if (sample_rate_hz == 0U || frame_size_bytes == 0U) {
        return UINT64_MAX;
    }

    uint64_t stream_bytes = seismic_recorder_saturating_multiply(
        (uint64_t)sample_rate_hz,
        (uint64_t)frame_size_bytes
    );
    stream_bytes = seismic_recorder_saturating_multiply(
        stream_bytes,
        (uint64_t)SEISMIC_RECORDER_START_RUNWAY_SECONDS
    );

    return seismic_recorder_saturating_add(
        stream_bytes,
        cache_reserve_bytes
    );
}


esp_err_t seismic_recorder_storage_preflight(
    uint32_t sample_rate_hz
)
{
    if (sample_rate_hz == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        return err;
    }

    sd_card_info_t info;
    err = sd_card_get_info(&info);

    storage_manager_operation_end();

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "RECORDER_PREFLIGHT result=query-failed err=%s",
            esp_err_to_name(err)
        );
        return err;
    }

    const uint64_t cache_reserve_bytes =
        (uint64_t)CONFIG_SEISMIC_RECORDER_PSRAM_CACHE_KB * 1024ULL;
    const uint64_t required_bytes =
        seismic_recorder_storage_required_bytes(
            sample_rate_hz,
            sizeof(seismic_frame_t),
            cache_reserve_bytes
        );

    if (info.free_bytes < required_bytes) {
        ESP_LOGE(
            TAG,
            "RECORDER_PREFLIGHT result=insufficient-space free=%llu required=%llu rate=%u frame=%u cache=%llu",
            (unsigned long long)info.free_bytes,
            (unsigned long long)required_bytes,
            (unsigned)sample_rate_hz,
            (unsigned)sizeof(seismic_frame_t),
            (unsigned long long)cache_reserve_bytes
        );
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "RECORDER_PREFLIGHT result=pass free=%llu required=%llu rate=%u runway_s=%u",
        (unsigned long long)info.free_bytes,
        (unsigned long long)required_bytes,
        (unsigned)sample_rate_hz,
        (unsigned)SEISMIC_RECORDER_START_RUNWAY_SECONDS
    );

    return ESP_OK;
}
