#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "seismic_buffer.h"
#include "seismic_recorder_lod_store.h"


/*
 * The web-pipeline host contract textually includes the production recorder
 * filesystem translation unit. Its quoted preflight header therefore resolves
 * to the production declaration before host include paths are considered.
 * Provide the external boundary here so the contract keeps exercising the
 * real filesystem path without pulling FATFS/SD-card policy into this test.
 */
esp_err_t seismic_recorder_storage_preflight(
    uint32_t sample_rate_hz
)
{
    return sample_rate_hz == 0U
        ? ESP_ERR_INVALID_ARG
        : ESP_OK;
}


bool seismic_recorder_cache_contains_session_range(
    const char *object_id,
    const char *session_id,
    uint64_t first_session_frame,
    uint64_t frame_count
)
{
    (void)object_id;
    (void)session_id;
    (void)first_session_frame;
    (void)frame_count;
    return false;
}


esp_err_t seismic_recorder_cache_copy_session_range(
    const char *object_id,
    const char *session_id,
    uint64_t first_session_frame,
    seismic_frame_t *frames,
    size_t frame_count
)
{
    (void)object_id;
    (void)session_id;
    (void)first_session_frame;
    (void)frames;
    (void)frame_count;
    return ESP_ERR_NOT_FOUND;
}


/* The web-pipeline contract exercises reader/routing behavior, not optional
 * index persistence. Keep the disposable LOD store disabled in this host
 * boundary so the production filesystem TU can still be linked unchanged. */
esp_err_t seismic_recorder_lod_store_begin(
    const char *session_path,
    uint32_t sample_rate_hz
)
{
    (void)session_path;
    (void)sample_rate_hz;
    return ESP_FAIL;
}


esp_err_t seismic_recorder_lod_store_append(
    const seismic_frame_t *persisted_frames,
    size_t frame_count
)
{
    (void)persisted_frames;
    (void)frame_count;
    return ESP_ERR_INVALID_STATE;
}


esp_err_t seismic_recorder_lod_store_flush(bool sync_to_media)
{
    (void)sync_to_media;
    return ESP_ERR_INVALID_STATE;
}


esp_err_t seismic_recorder_lod_store_close(void)
{
    return ESP_OK;
}


void seismic_recorder_lod_store_abort(void)
{
}


bool seismic_recorder_lod_store_is_active(void)
{
    return false;
}
