#include <string.h>

#include "seismic_recorder.h"

esp_err_t seismic_recorder_read_session_range_ex(
    const char *object_id,
    const char *session_id,
    uint64_t start_index,
    size_t max_frames,
    seismic_frame_t *frames,
    uint64_t *source_sample_indices,
    bool *gap_before,
    size_t *frame_count_out,
    seismic_recorder_session_summary_t *summary_out,
    seismic_recorder_raw_window_stats_t *raw_stats_out
)
{
    if (raw_stats_out != NULL) {
        memset(raw_stats_out, 0, sizeof(*raw_stats_out));
    }

    const esp_err_t err = seismic_recorder_read_session_range(
        object_id, session_id, start_index, max_frames, frames,
        NULL, gap_before, frame_count_out, summary_out
    );
    if (err != ESP_OK) {
        return err;
    }

    if (source_sample_indices != NULL) {
        for (size_t i = 0U; i < *frame_count_out; ++i) {
            source_sample_indices[i] = start_index + (uint64_t)i;
        }
    }
    if (gap_before != NULL) {
        for (size_t i = 0U; i < *frame_count_out; ++i) {
            gap_before[i] = false;
        }
    }
    return ESP_OK;
}
