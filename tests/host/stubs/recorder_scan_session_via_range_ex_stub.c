#include <stdlib.h>

#include "seismic_recorder.h"

/*
 * Host stand-in for the real sequential session scan: visits the whole session in capacity-sized chunks through the
 * test's own seismic_recorder_read_session_range_ex fake, so the test fixture stays the single source of frames.
 * The fakes never report gaps, so every chunk is one contiguous source-index run.
 */
esp_err_t seismic_recorder_scan_session(
    const char *object_id,
    const char *session_id,
    seismic_frame_t *buffer,
    size_t capacity,
    seismic_recorder_scan_callback_t callback,
    void *context
)
{
    if (buffer == NULL || capacity == 0U || callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    bool *gaps = malloc(capacity * sizeof(*gaps));
    if (gaps == NULL) {
        return ESP_ERR_NO_MEM;
    }
    uint64_t start = 0U;
    esp_err_t err = ESP_OK;
    for (;;) {
        size_t count = 0U;
        err = seismic_recorder_read_session_range_ex(
            object_id, session_id, start, capacity, buffer, NULL, gaps, &count, NULL, NULL
        );
        if (err != ESP_OK || count == 0U) {
            break;
        }
        err = callback(buffer, count, start, gaps[0], context);
        if (err != ESP_OK) {
            break;
        }
        start += count;
    }
    free(gaps);
    return err;
}
