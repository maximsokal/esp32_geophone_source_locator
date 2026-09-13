#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "seismic_recorder_internal.h"
#include "seismic_recorder_lod.h"


typedef struct
{
    uint64_t raw_frames_read;
    uint64_t lod_records_read;
    uint64_t lod_frames_read;

} seismic_recorder_lod_read_stats_t;


/*
 * Exact hybrid reduction over samples.bin + waveform-lod64.bin.
 *
 * A LOD64 record is used only when all 64 represented raw frames belong to
 * the requested range and to one output min/max bucket. Blocks that intersect
 * a query edge or an output-bucket boundary are read from samples.bin instead.
 * This guarantees the same point partitioning as the raw reducer: the sidecar
 * changes I/O volume, never waveform semantics.
 */
esp_err_t seismic_recorder_lod_read_source(
    seismic_recorder_waveform_read_at_fn raw_read_at,
    void *raw_context,
    uint64_t raw_file_size,
    seismic_recorder_waveform_read_at_fn lod_read_at,
    void *lod_context,
    uint64_t lod_file_size,
    const seismic_recorder_session_summary_t *summary,
    uint64_t start_frame,
    uint64_t frame_count,
    size_t max_points,
    seismic_frame_t *raw_staging,
    size_t raw_staging_capacity,
    seismic_recorder_lod_record_t *lod_staging,
    size_t lod_staging_capacity,
    seismic_recorder_waveform_point_t *points,
    size_t point_capacity,
    seismic_recorder_waveform_info_t *info
);


/* Same exact reducer with optional execution statistics. The counters describe
 * only waveform payload consumed after all validation succeeds far enough to
 * read it: LOD header validation does not count as LOD-record use. This makes
 * lod_records_read > 0 a precise proof that waveform-lod64.bin replaced raw
 * frames for this request rather than merely being present and valid. */
esp_err_t seismic_recorder_lod_read_source_with_stats(
    seismic_recorder_waveform_read_at_fn raw_read_at,
    void *raw_context,
    uint64_t raw_file_size,
    seismic_recorder_waveform_read_at_fn lod_read_at,
    void *lod_context,
    uint64_t lod_file_size,
    const seismic_recorder_session_summary_t *summary,
    uint64_t start_frame,
    uint64_t frame_count,
    size_t max_points,
    seismic_frame_t *raw_staging,
    size_t raw_staging_capacity,
    seismic_recorder_lod_record_t *lod_staging,
    size_t lod_staging_capacity,
    seismic_recorder_waveform_point_t *points,
    size_t point_capacity,
    seismic_recorder_waveform_info_t *info,
    seismic_recorder_lod_read_stats_t *stats
);


bool seismic_recorder_lod_block_maps_to_one_point(
    uint64_t request_start_frame,
    uint64_t request_frame_count,
    size_t point_count,
    uint64_t block_first_session_frame,
    size_t *point_index
);
