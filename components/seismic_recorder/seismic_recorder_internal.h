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
#define SEISMIC_RECORDER_BATCH_FRAMES         512U
#define SEISMIC_RECORDER_WRITE_STAGING_FRAMES 256U
#define SEISMIC_RECORDER_READ_STAGING_FRAMES  1024U
/*
 * Acquisition is copied into a bounded PSRAM write-behind cache before SD.
 * The recorder drains at most one 256-frame storage chunk per worker turn,
 * returning immediately to acquisition ingestion. The filesystem writer then
 * copies that PSRAM chunk through its internal DMA_ATTR staging buffer so
 * SDSPI remains on a DMA-capable multi-block path.
 *
 * Historical waveform reads use a separate PSRAM staging buffer.  The public
 * reader re-opens/seeks/reads/closes samples.bin inside one bounded storage
 * window per staging chunk, then performs min/max reduction after the SPI2
 * lock is released.  This avoids holding TFT quiesce across an arbitrarily
 * large requested history range while CONFIG_FATFS_FS_LOCK>=7 remains safe:
 * no FILE handle survives outside its storage arbitration window. If a whole
 * requested range is still resident in the recorder cache, the reader may use
 * the retained PSRAM session copy and avoid SD/SPI2 completely.
 */
#define SEISMIC_RECORDER_FLUSH_INTERVAL_US    30000000LL
#define SEISMIC_RECORDER_IDLE_DELAY_MS         10
#define SEISMIC_RECORDER_METADATA_BUFFER_SIZE  2048
#define SEISMIC_RECORDER_BINARY_VERSION        1U
#define SEISMIC_RECORDER_SESSION_SCHEMA_VERSION_V1 1U
#define SEISMIC_RECORDER_SESSION_SCHEMA_VERSION_V2 2U


typedef struct
{
    char magic[8];
    uint16_t version;
    uint16_t channel_count;
    uint32_t sample_rate_hz;
    uint32_t frame_size_bytes;
    uint32_t reserved;

} seismic_recorder_binary_header_t;


typedef esp_err_t (*seismic_recorder_waveform_read_at_fn)(
    void *context,
    uint64_t offset_bytes,
    void *buffer,
    size_t size_bytes
);


bool seismic_recorder_cache_contains_session_range(
    const char *object_id,
    const char *session_id,
    uint64_t first_session_frame,
    uint64_t frame_count
);


esp_err_t seismic_recorder_cache_copy_session_range(
    const char *object_id,
    const char *session_id,
    uint64_t first_session_frame,
    seismic_frame_t *frames,
    size_t frame_count
);


esp_err_t seismic_recorder_fs_build_session_path(
    const char *object_id,
    const char *session_id,
    char *path,
    size_t path_size
);


esp_err_t seismic_recorder_waveform_read_source(
    seismic_recorder_waveform_read_at_fn read_at,
    void *read_context,
    uint64_t file_size,
    const seismic_recorder_session_summary_t *summary,
    uint64_t start_frame,
    uint64_t frame_count,
    size_t max_points,
    seismic_frame_t *staging,
    size_t staging_capacity,
    seismic_recorder_waveform_point_t *points,
    size_t point_capacity,
    seismic_recorder_waveform_info_t *info
);


esp_err_t seismic_recorder_waveform_read_file(
    FILE *samples_file,
    uint64_t file_size,
    const seismic_recorder_session_summary_t *summary,
    uint64_t start_frame,
    uint64_t frame_count,
    size_t max_points,
    seismic_recorder_waveform_point_t *points,
    size_t point_capacity,
    seismic_recorder_waveform_info_t *info
);


esp_err_t seismic_recorder_waveform_read_raw_source(
    seismic_recorder_waveform_read_at_fn read_at,
    void *read_context,
    uint64_t file_size,
    const seismic_recorder_session_summary_t *summary,
    uint64_t start_frame,
    uint64_t frame_count,
    seismic_frame_t *staging,
    size_t staging_capacity,
    seismic_frame_t *frames,
    size_t frame_capacity,
    size_t *frame_count_out
);


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


esp_err_t seismic_recorder_fs_sync(
    FILE *samples_file
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
    const char *status,
    const seismic_recorder_session_participation_t *participation
);


esp_err_t seismic_recorder_json_read_summary(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_summary_t *summary
);


esp_err_t seismic_recorder_json_read_summary_known_present(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_summary_t *summary
);


esp_err_t seismic_recorder_json_parse_participation_text(
    const char *json,
    seismic_recorder_session_participation_t *participation
);
