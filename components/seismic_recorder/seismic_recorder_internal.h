#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "object_store.h"
#include "seismic_acquisition.h"
#include "seismic_buffer.h"
#include "seismic_recorder.h"


#define SEISMIC_RECORDER_PATH_SIZE            256
#define SEISMIC_RECORDER_BATCH_FRAMES         512U
#define SEISMIC_RECORDER_WRITE_STAGING_FRAMES 256U
#define SEISMIC_RECORDER_READ_STAGING_FRAMES  1024U
#define SEISMIC_RECORDER_BINARY_VERSION_V1      1U
#define SEISMIC_RECORDER_BINARY_VERSION_V2      2U
#define SEISMIC_RECORDER_BINARY_VERSION         SEISMIC_RECORDER_BINARY_VERSION_V1
#define SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES (SEISMIC_CHANNEL_COUNT * sizeof(int16_t))
#define SEISMIC_RECORDER_V2_METADATA_BYTES      44U
#define SEISMIC_RECORDER_V2_BLOCK_HEADER_BYTES  20U
#define SEISMIC_RECORDER_V2_BLOCK_MAX_FRAMES    256U
#define SEISMIC_RECORDER_V2_BLOCK_FLAG_GAP_BEFORE 0x0001U
/*
 * Acquisition is copied into a bounded PSRAM write-behind cache before SD.
 * The recorder drains at most one 256-frame storage chunk per worker turn,
 * returning immediately to acquisition ingestion. The filesystem writer then
 * copies that PSRAM chunk through its internal DMA_ATTR staging buffer so
 * SDSPI remains on a DMA-capable multi-block path.
 *
 * Historical waveform reads use a separate PSRAM staging buffer.  The public
 * reader decodes v1 float records or v2 int16 blocks inside bounded storage
 * windows, then performs min/max reduction after the SPI2 lock is released.
 * A v2 read also carries source sample indexes so gaps remain observable.
 */
#define SEISMIC_RECORDER_FLUSH_INTERVAL_US    30000000LL
#define SEISMIC_RECORDER_IDLE_DELAY_MS         10
#define SEISMIC_RECORDER_METADATA_BUFFER_SIZE  8192
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


/*
 * GEOSEIS1 v1 is the 24-byte base header followed by native ESP little-endian
 * seismic_frame_t records (int64 timestamp_us, then four float32 µV values).
 * v2 keeps that 24-byte prefix, then adds 44 bytes: four float32 scales in
 * µV/LSB, four float32 offsets in µV, float32 input_range_v, uint32 OS ratio,
 * and uint32 SPI clock. The payload is repeated 20-byte block headers
 * (uint64 first_sample_index, uint64 first_timestamp_us, uint16 frame_count,
 * uint16 flags; bit 0 means a gap precedes this block), followed by
 * frame_count interleaved groups of four int16 channel codes. Integers and
 * floats are little-endian. v2 base frame_size_bytes is eight.
 */
#pragma pack(push, 1)
typedef struct
{
    float scale_uv_per_lsb[SEISMIC_CHANNEL_COUNT];
    float offset_uv[SEISMIC_CHANNEL_COUNT];
    float input_range_v;
    uint32_t oversampling_ratio;
    uint32_t spi_clock_hz;

} seismic_recorder_binary_v2_metadata_t;


typedef struct
{
    uint64_t first_sample_index;
    uint64_t first_timestamp_us;
    uint16_t frame_count;
    uint16_t flags;

} seismic_recorder_binary_v2_block_header_t;
#pragma pack(pop)


_Static_assert(
    sizeof(seismic_recorder_binary_header_t) == 24U,
    "GEOSEIS1 base header layout changed"
);
_Static_assert(
    sizeof(seismic_recorder_binary_v2_metadata_t) ==
        SEISMIC_RECORDER_V2_METADATA_BYTES,
    "GEOSEIS1 v2 metadata layout changed"
);
_Static_assert(
    sizeof(seismic_recorder_binary_v2_block_header_t) ==
        SEISMIC_RECORDER_V2_BLOCK_HEADER_BYTES,
    "GEOSEIS1 v2 block header layout changed"
);


typedef struct seismic_recorder_sample_format
{
    uint16_t version;
    uint32_t sample_rate_hz;
    float scale_uv_per_lsb[SEISMIC_CHANNEL_COUNT];
    float offset_uv[SEISMIC_CHANNEL_COUNT];
    float input_range_v;
    uint32_t oversampling_ratio;
    uint32_t spi_clock_hz;

} seismic_recorder_sample_format_t;


typedef struct
{
    seismic_recorder_binary_header_t header;
    seismic_recorder_binary_v2_metadata_t v2_metadata;
    uint64_t file_size_bytes;
    uint64_t data_offset_bytes;
    uint64_t logical_frame_count;
    uint64_t logical_frame_position;
    uint64_t next_block_offset_bytes;
    uint64_t block_first_sample_index;
    uint64_t block_first_timestamp_us;
    uint64_t block_data_offset_bytes;
    uint64_t previous_sample_index;
    uint64_t previous_timestamp_us;
    uint64_t missing_sample_count;
    uint16_t block_frame_count;
    uint16_t block_consumed;
    uint16_t block_flags;
    bool block_loaded;
    bool have_previous_sample;
    bool has_gaps;
    bool require_exact_file_size;

} seismic_recorder_samples_reader_t;


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


esp_err_t seismic_recorder_samples_format_from_acquisition(
    const seismic_acquisition_snapshot_t *acquisition,
    seismic_recorder_sample_format_t *format
);


void seismic_recorder_samples_v2_metadata_from_format(
    const seismic_recorder_sample_format_t *format,
    seismic_recorder_binary_v2_metadata_t *metadata
);


esp_err_t seismic_recorder_samples_encode_v2_block(
    const seismic_recorder_sample_format_t *format,
    const seismic_frame_t *frames,
    const uint64_t *sample_indices,
    size_t frame_count,
    bool gap_before,
    void *buffer,
    size_t buffer_capacity,
    size_t *bytes_written
);


esp_err_t seismic_recorder_samples_quantize_frame(
    const seismic_recorder_sample_format_t *format,
    const seismic_frame_t *frame,
    seismic_frame_t *quantized_frame
);


bool seismic_recorder_samples_timestamp_at(
    uint64_t first_timestamp_us,
    uint64_t sample_offset,
    uint32_t sample_rate_hz,
    uint64_t *timestamp_us
);


esp_err_t seismic_recorder_samples_reader_open(
    seismic_recorder_waveform_read_at_fn read_at,
    void *read_context,
    uint64_t file_size,
    const seismic_recorder_session_summary_t *summary,
    seismic_recorder_samples_reader_t *reader
);


esp_err_t seismic_recorder_samples_reader_skip(
    seismic_recorder_waveform_read_at_fn read_at,
    void *read_context,
    seismic_recorder_samples_reader_t *reader,
    uint64_t frame_count
);


esp_err_t seismic_recorder_samples_reader_next(
    seismic_recorder_waveform_read_at_fn read_at,
    void *read_context,
    seismic_recorder_samples_reader_t *reader,
    seismic_frame_t *frames,
    size_t frame_capacity,
    size_t *frame_count,
    uint64_t *first_sample_index,
    bool *gap_before
);


/* Add raw v2 rail counts and examined sample frames to the supplied outputs.
 * The caller initializes them to zero before the first call. */
esp_err_t seismic_recorder_samples_reader_next_ex(
    seismic_recorder_waveform_read_at_fn read_at,
    void *read_context,
    seismic_recorder_samples_reader_t *reader,
    seismic_frame_t *frames,
    size_t frame_capacity,
    size_t *frame_count,
    uint64_t *first_sample_index,
    bool *gap_before,
    size_t rail_sample_counts[SEISMIC_CHANNEL_COUNT],
    size_t *samples_examined
);


esp_err_t seismic_recorder_samples_reader_finish(
    seismic_recorder_waveform_read_at_fn read_at,
    void *read_context,
    seismic_recorder_samples_reader_t *reader
);


esp_err_t seismic_recorder_samples_recover_file(
    FILE *file,
    uint64_t file_size,
    uint32_t expected_sample_rate_hz,
    uint16_t expected_channel_count,
    uint64_t *frame_count,
    uint64_t *target_size_bytes,
    uint16_t *format_version
);


esp_err_t seismic_recorder_fs_create_session(
    const char *object_id,
    const seismic_acquisition_snapshot_t *acquisition,
    uint8_t channel_count,
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE],
    FILE **samples_file,
    uint16_t *format_version
);


esp_err_t seismic_recorder_fs_write_frames(
    FILE *samples_file,
    const seismic_frame_t *frames,
    const uint64_t *sample_indices,
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


/*
 * Summary-read errors that describe the metadata itself (absent, malformed,
 * oversized). A listing may show such a session as incomplete. Anything else
 * (ESP_ERR_NO_MEM, I/O or lock failures) says nothing about the session, so the
 * listing must report it instead of presenting a healthy session as unfinished.
 */
static inline bool seismic_recorder_summary_error_is_unreadable_metadata(
    esp_err_t err
)
{
    return err == ESP_ERR_NOT_FOUND ||
        err == ESP_ERR_INVALID_RESPONSE ||
        err == ESP_ERR_INVALID_SIZE;
}


esp_err_t seismic_recorder_json_parse_participation_text(
    const char *json,
    seismic_recorder_session_participation_t *participation
);
