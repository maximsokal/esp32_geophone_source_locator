#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "seismic_buffer.h"


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
