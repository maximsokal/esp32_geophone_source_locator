#include "seismic_recorder_lod_store.h"


static bool s_active = false;


esp_err_t seismic_recorder_lod_store_begin(
    const char *session_path,
    uint32_t sample_rate_hz
)
{
    if (session_path == NULL || sample_rate_hz == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    s_active = true;
    return ESP_OK;
}


esp_err_t seismic_recorder_lod_store_append(
    const seismic_frame_t *persisted_frames,
    size_t frame_count
)
{
    return
        s_active && persisted_frames != NULL && frame_count > 0U
            ? ESP_OK
            : ESP_ERR_INVALID_STATE;
}


esp_err_t seismic_recorder_lod_store_flush(bool sync_to_media)
{
    (void)sync_to_media;
    return s_active ? ESP_OK : ESP_ERR_INVALID_STATE;
}


esp_err_t seismic_recorder_lod_store_close(void)
{
    s_active = false;
    return ESP_OK;
}


void seismic_recorder_lod_store_abort(void)
{
    s_active = false;
}


bool seismic_recorder_lod_store_is_active(void)
{
    return s_active;
}
