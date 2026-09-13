#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "seismic_buffer.h"


/*
 * First persisted waveform LOD tier.
 *
 * One record summarizes exactly 64 consecutive *session* frames. Session
 * frames are dense samples.bin positions, not producer/source frame numbers;
 * acquisition gaps therefore remain represented by timestamp gaps and the
 * session metadata dropped_frames counter without creating holes in this
 * index.
 *
 * The sidecar is explicitly disposable. samples.bin remains authoritative.
 */
#define SEISMIC_RECORDER_LOD_FILE_NAME          "waveform-lod64.bin"
#define SEISMIC_RECORDER_LOD_BLOCK_FRAMES      64U
#define SEISMIC_RECORDER_LOD_FILE_VERSION       1U
#define SEISMIC_RECORDER_LOD_FILE_HEADER_BYTES 32U
#define SEISMIC_RECORDER_LOD_RECORD_BYTES      64U
#define SEISMIC_RECORDER_LOD_FLAG_COMPLETE      0x00000001U


typedef struct
{
    char magic[8];
    uint16_t version;
    uint16_t channel_count;
    uint32_t block_frames;
    uint32_t sample_rate_hz;
    uint32_t frame_size_bytes;
    uint32_t record_size_bytes;
    uint32_t flags;

} seismic_recorder_lod_file_header_t;


typedef struct
{
    uint64_t first_session_frame;
    int64_t start_timestamp_us;
    int64_t end_timestamp_us;
    uint32_t frame_count;
    uint32_t reserved;
    float min_uv[SEISMIC_CHANNEL_COUNT];
    float max_uv[SEISMIC_CHANNEL_COUNT];

} seismic_recorder_lod_record_t;


typedef struct
{
    uint64_t next_session_frame;
    uint64_t pending_first_session_frame;
    uint32_t pending_count;
    int64_t pending_start_timestamp_us;
    int64_t pending_end_timestamp_us;
    float pending_min_uv[SEISMIC_CHANNEL_COUNT];
    float pending_max_uv[SEISMIC_CHANNEL_COUNT];

} seismic_recorder_lod_builder_t;


void seismic_recorder_lod_header_init(
    seismic_recorder_lod_file_header_t *header,
    uint32_t sample_rate_hz
);


bool seismic_recorder_lod_header_valid(
    const seismic_recorder_lod_file_header_t *header,
    uint32_t expected_sample_rate_hz
);


void seismic_recorder_lod_header_mark_complete(
    seismic_recorder_lod_file_header_t *header
);


bool seismic_recorder_lod_header_is_complete(
    const seismic_recorder_lod_file_header_t *header
);


bool seismic_recorder_lod_expected_file_size(
    uint64_t persisted_session_frames,
    uint64_t *size_bytes
);


void seismic_recorder_lod_builder_reset(
    seismic_recorder_lod_builder_t *builder,
    uint64_t next_session_frame
);


/*
 * Consume newly-persisted samples.bin frames in strict session order.
 *
 * Only complete 64-frame records are emitted. The incomplete tail remains in
 * builder state and is intentionally omitted from the sidecar; readers use
 * raw samples.bin for such boundary frames. The call is transactional for
 * validation/capacity failures: builder and records remain untouched.
 */
esp_err_t seismic_recorder_lod_builder_append(
    seismic_recorder_lod_builder_t *builder,
    uint64_t first_session_frame,
    const seismic_frame_t *frames,
    size_t frame_count,
    seismic_recorder_lod_record_t *records,
    size_t record_capacity,
    size_t *record_count
);
