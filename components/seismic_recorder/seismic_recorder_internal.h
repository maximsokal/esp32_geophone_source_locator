#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "object_store.h"
#include "seismic_buffer.h"
#include "seismic_recorder.h"


#define SEISMIC_RECORDER_PATH_SIZE            256
#define SEISMIC_RECORDER_BATCH_FRAMES         128
#define SEISMIC_RECORDER_FLUSH_INTERVAL_US     500000LL
#define SEISMIC_RECORDER_IDLE_DELAY_MS         10
#define SEISMIC_RECORDER_STOP_TIMEOUT_MS       5000
#define SEISMIC_RECORDER_METADATA_BUFFER_SIZE  2048


typedef struct
{
    char magic[8];
    uint16_t version;
    uint16_t channel_count;
    uint32_t sample_rate_hz;
    uint32_t frame_size_bytes;
    uint32_t reserved;

} seismic_recorder_binary_header_t;


esp_err_t seismic_recorder_fs_create_session(
    const char *object_id,
    uint32_t sample_rate_hz,
    uint8_t channel_count,
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE],
    FILE **samples_file
);


esp_err_t seismic_recorder_fs_write_frames(
    FILE *samples_file,
    const seismic_frame_t *frames,
    size_t frame_count,
    size_t *frames_written
);


esp_err_t seismic_recorder_fs_flush(
    FILE *samples_file,
    bool sync_to_media
);


esp_err_t seismic_recorder_fs_close(
    FILE *samples_file
);


esp_err_t seismic_recorder_fs_list_sessions(
    const char *object_id,
    seismic_recorder_session_summary_t *sessions,
    size_t max_sessions,
    size_t *session_count
);


esp_err_t seismic_recorder_fs_delete_session(
    const char *object_id,
    const char *session_id
);


esp_err_t seismic_recorder_json_write_metadata(
    const object_store_profile_t *profile,
    const seismic_recorder_snapshot_t *snapshot,
    const char *status
);


esp_err_t seismic_recorder_json_read_summary(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_summary_t *summary
);
