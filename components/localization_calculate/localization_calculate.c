#include "localization_calculate.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "localization_bayes.h"
#include "localization_calculate_policy.h"
#include "localization_eikonal.h"
#include "localization_event_run_record.h"
#include "localization_psafuvs.h"
#include "localization_psp.h"
#include "localization_robust.h"
#include "localization_run_store.h"
#include "localization_suea.h"
#include "localization_tdcas.h"
#include "localization_xcorr_tdoa.h"
#include "seismic_dsp.h"
#include "scan_occupancy.h"
#include "seismic_recorder.h"
#include "storage_manager.h"
#ifdef ESP_PLATFORM
#include "power_control.h"
#include "storage_bulk_admission.h"
#endif


#ifndef CONFIG_LOCALIZATION_MAX_ANALYSIS_S
#define CONFIG_LOCALIZATION_MAX_ANALYSIS_S 10
#endif

#ifndef CONFIG_LOCALIZATION_ANALYSIS_PSRAM_BUDGET_KB
#define CONFIG_LOCALIZATION_ANALYSIS_PSRAM_BUDGET_KB 3072
#endif

/* A numerical search-strategy knob (coarse-to-refine grid narrowing), not a
 * physical measurement -- see localization_calculate_search_bounds()'s own
 * doc comment for the same distinction applied to the search box itself.
 * Both grid-search backends require refine_divisor >= 2. */
#define LOCALIZATION_CALCULATE_REFINE_DIVISOR 4U

#define LOCALIZATION_CALCULATE_METHOD_VERSION 1U
/* Covers the 1 MiB occupancy file, up to 1.33 MiB expanded keys, workspace,
 * and allocator margin while the analysis arrays are resident. */
#define LOCALIZATION_ANALYSIS_PSRAM_SAFETY_RESERVE_BYTES (3072U * 1024U)

static const char *TAG = "LOCALIZATION_CALCULATE";

#define LOCALIZATION_CALCULATE_ALL_CHANNELS_MASK \
    ((1U << OBJECT_STORE_POD_COUNT) - 1U)

#define LOCALIZATION_CALCULATE_BASE_METHODS 12U
#define LOCALIZATION_CALCULATE_SCAN_METHODS 8U
#define LOCALIZATION_CALCULATE_FUSION_METHODS 1U

/* Twelve unconstrained methods (fused) plus up to eight scan-constrained
 * variants (persisted for comparison, never fused). */
#define LOCALIZATION_CALCULATE_MAX_RECORDS 20U

/* Budgets of the extended methods on the device (a numerical search
 * strategy, not physics): eikonal lattice nodes, Bayesian cells per level,
 * velocity quadrature nodes, and the seeding lattices. */
#define LOCALIZATION_CALCULATE_EIKONAL_MAX_NODES 40000U
#define LOCALIZATION_CALCULATE_BAYES_MAX_CELLS 8000U
#define LOCALIZATION_CALCULATE_BAYES_VELOCITY_NODES 5U
#define LOCALIZATION_CALCULATE_SEED_LATTICE_POINTS 6000U

/* Forward-model error of the eikonal travel times, fed to the Bayesian
 * posterior so its region covers the model as well as the picks: the
 * fast-marching scheme's own error (host tests: <= 1.43 % of the travel
 * time in a homogeneous medium, 5-cell exact source ball) and a floor for
 * a void edge discretized at the lattice or scan cell, which can move a
 * path at each of ~two corners by up to one cell: cell / (v sqrt 6). */
#define LOCALIZATION_CALCULATE_EIKONAL_RELATIVE_ERROR 0.015

/* Unknowns of the velocity-free fits: TDCAS (x, y, z, t0, v) over N P
 * picks; PSAFUVS (x, y, z, t0, vP, vS) over 2N P and S picks. */
#define LOCALIZATION_CALCULATE_TDCAS_UNKNOWNS 5U
#define LOCALIZATION_CALCULATE_PSAFUVS_UNKNOWNS 6U
#define LOCALIZATION_SCAN_IDENTITY_CHUNK_BYTES (4U * 1024U)


/*
 * The run's records and fusion state live on the heap, not the stack:
 * localization_calculate_run() runs on the 8 KiB HTTPS task, and the
 * records alone are ~4 KiB.
 */
typedef struct
{
    localization_run_record_t records[LOCALIZATION_CALCULATE_MAX_RECORDS];
    localization_comparison_method_result_t comparison_methods[LOCALIZATION_COMPARISON_MAX_METHODS];
    localization_comparison_result_t comparison_result;

    /* Velocity-free methods' inputs and results. */
    localization_observation_t p_observations[OBJECT_STORE_POD_COUNT];
    localization_psafuvs_observation_t ps_observations[OBJECT_STORE_POD_COUNT];
    /* Observation index -> channel: the observation arrays skip channels
     * without the needed picks. */
    size_t p_channel[OBJECT_STORE_POD_COUNT];
    size_t ps_channel[OBJECT_STORE_POD_COUNT];
    localization_suea_pick_t suea_picks[OBJECT_STORE_POD_COUNT];
    localization_tdcas_result_t tdcas;
    localization_psafuvs_result_t psafuvs;
    localization_suea_result_t suea;
    localization_tdcas_result_t scan_tdcas;
    localization_psafuvs_result_t scan_psafuvs;
    localization_suea_result_t scan_suea;
    localization_suea_grid_t suea_grid;

    /* S picks by channel (for PSAFUVS and PSP). */
    bool s_valid[OBJECT_STORE_POD_COUNT];
    int64_t s_arrival_us[OBJECT_STORE_POD_COUNT];
    float s_snr[OBJECT_STORE_POD_COUNT];

    /* localization_extended. Participant index -> channel for the methods
     * that take every participating pod (xcorr, eikonal, Bayesian). */
    size_t participant_channel[OBJECT_STORE_POD_COUNT];
    localization_point3d_t eikonal_sensors[OBJECT_STORE_POD_COUNT];
    localization_xcorr_channel_t xcorr_channels[OBJECT_STORE_POD_COUNT];
    localization_xcorr_pair_t xcorr_pairs[LOCALIZATION_XCORR_MAX_PAIRS];
    localization_xcorr_tdoa_result_t xcorr;
    localization_robust_result_t robust;
    localization_psp_observation_t psp_observations[OBJECT_STORE_POD_COUNT];
    localization_psp_result_t psp;
    localization_bayes_pick_t bayes_picks[OBJECT_STORE_POD_COUNT];
    localization_bayes_result_t bayes;
    localization_bayes_result_t scan_bayes;
    localization_eikonal_pick_t eikonal_picks[OBJECT_STORE_POD_COUNT];
    localization_eikonal_model_t eikonal_model;
    localization_eikonal_fields_t eikonal_fields;
    localization_eikonal_result_t eikonal;
    localization_eikonal_result_t scan_eikonal;
    localization_eikonal_travel_time_context_t eikonal_times;

    /* Kept here, not on the stack, for the same reason. */
    localization_pick_record_t pick_record;
    localization_run_record_t joint_record;

} localization_calculate_workspace_t;


/* An observation-indexed mask (bit i: observations[i]) as a channel mask. */
static uint32_t observation_mask_to_channels(uint32_t observation_mask, const size_t *channel, size_t count)
{
    uint32_t channels = 0U;
    for (size_t i = 0U; i < count; ++i) {
        if ((observation_mask & (1U << i)) != 0U) {
            channels |= 1U << channel[i];
        }
    }
    return channels;
}


/*
 * MAX-147/162: loads the compact scan occupancy saved beside THIS session's
 * scan (never another session's or the object's latest). ESP_ERR_NOT_FOUND
 * when the session has no occupancy artifact.
 */
static esp_err_t load_session_occupancy(
    const char *object_id,
    const char *session_id,
    scan_occupancy_t *occupancy
)
{
    char path[192];
    const int written = snprintf(
        path, sizeof(path), "%s/%s/sessions/%s/occupancy-v1.bin",
        STORAGE_MANAGER_OBJECTS_PATH, object_id, session_id
    );
    if (written <= 0 || (size_t)written >= sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        return err;
    }
    uint8_t *data = NULL;
    size_t length = 0U;
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        err = ESP_ERR_NOT_FOUND;
    } else {
        long size = -1L;
        if (fseek(file, 0L, SEEK_END) == 0) {
            size = ftell(file);
        }
        if (size <= 0L || (unsigned long)size > SCAN_OCCUPANCY_MAX_BYTES || fseek(file, 0L, SEEK_SET) != 0) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            data = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (data == NULL) {
                err = ESP_ERR_NO_MEM;
            } else if (fread(data, 1U, (size_t)size, file) != (size_t)size) {
                err = ESP_FAIL;
            } else {
                length = (size_t)size;
            }
        }
        fclose(file);
    }
    storage_manager_operation_end();

    if (err == ESP_OK) {
        err = scan_occupancy_parse(data, length, occupancy);
    }
    heap_caps_free(data);
    return err;
}


typedef struct
{
    FILE *file;
    uint8_t *buffer;
    size_t requested_bytes;
    size_t bytes_read;
} localization_scan_identity_read_t;


static uint32_t localization_scan_crc32_update(
    uint32_t crc,
    const uint8_t *bytes,
    size_t length
)
{
    for (size_t i = 0U; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return crc;
}


static esp_err_t localization_scan_identity_read_chunk(void *context)
{
    localization_scan_identity_read_t *read = (localization_scan_identity_read_t *)context;
    if (read == NULL || read->file == NULL || read->buffer == NULL || read->requested_bytes == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
#ifdef ESP_PLATFORM
    if (power_control_is_shutdown_requested()) {
        return ESP_ERR_INVALID_STATE;
    }
#endif
    read->bytes_read = fread(read->buffer, 1U, read->requested_bytes, read->file);
    return read->bytes_read == read->requested_bytes && !ferror(read->file)
        ? ESP_OK
        : ESP_FAIL;
}


static esp_err_t localization_scan_identity_run_chunk(
    localization_scan_identity_read_t *read
)
{
#ifdef ESP_PLATFORM
    return storage_bulk_admission_run_chunk(localization_scan_identity_read_chunk, read);
#else
    return localization_scan_identity_read_chunk(read);
#endif
}


/*
 * The host harness supplies scan-v1.bin beside its occupancy fixture under
 * STORAGE_MANAGER_OBJECTS_PATH, the same I/O seam used by load_session_occupancy.
 * On device, each bounded read is admitted independently so active bulk work
 * can pause identity calculation and a pending shutdown stops at a chunk edge.
 */
static esp_err_t load_session_scan_frame_id(
    const char *object_id,
    const char *session_id,
    char frame_id[SCAN_OCCUPANCY_FRAME_ID_SIZE]
)
{
    if (object_id == NULL || session_id == NULL || frame_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[192];
    const int written = snprintf(
        path, sizeof(path), "%s/%s/sessions/%s/scan-v1.bin",
        STORAGE_MANAGER_OBJECTS_PATH, object_id, session_id
    );
    if (written <= 0 || (size_t)written >= sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        return err;
    }

    FILE *file = fopen(path, "rb");
    uint8_t *buffer = NULL;
    if (file == NULL) {
        err = ESP_ERR_NOT_FOUND;
    } else {
        long file_size = -1L;
        if (fseek(file, 0L, SEEK_END) == 0) {
            file_size = ftell(file);
        }
        if (file_size <= 0L || fseek(file, 0L, SEEK_SET) != 0) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            buffer = heap_caps_malloc(
                LOCALIZATION_SCAN_IDENTITY_CHUNK_BYTES,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
            );
            if (buffer == NULL) {
                err = ESP_ERR_NO_MEM;
            } else {
                uint32_t crc = 0xFFFFFFFFU;
                size_t remaining = (size_t)file_size;
                while (remaining > 0U && err == ESP_OK) {
                    const size_t chunk_bytes = remaining < LOCALIZATION_SCAN_IDENTITY_CHUNK_BYTES
                        ? remaining
                        : LOCALIZATION_SCAN_IDENTITY_CHUNK_BYTES;
                    localization_scan_identity_read_t read = {
                        .file = file,
                        .buffer = buffer,
                        .requested_bytes = chunk_bytes,
                        .bytes_read = 0U,
                    };
                    err = localization_scan_identity_run_chunk(&read);
                    if (err == ESP_OK) {
                        crc = localization_scan_crc32_update(crc, buffer, read.bytes_read);
                        remaining -= read.bytes_read;
                    }
                }
                if (err == ESP_OK) {
                    crc ^= 0xFFFFFFFFU;
                    const int frame_written = snprintf(
                        frame_id, SCAN_OCCUPANCY_FRAME_ID_SIZE,
                        "scan-crc32:%08x", (unsigned)crc
                    );
                    if (frame_written <= 0 ||
                        (size_t)frame_written >= SCAN_OCCUPANCY_FRAME_ID_SIZE) {
                        err = ESP_ERR_INVALID_SIZE;
                    }
                }
            }
        }
        if (fclose(file) != 0 && err == ESP_OK) {
            err = ESP_FAIL;
        }
    }

    heap_caps_free(buffer);
    storage_manager_operation_end();
    if (err != ESP_OK) {
        frame_id[0] = '\0';
    }
    return err;
}


static esp_err_t session_occupancy_probe(const char *object_id, const char *session_id)
{
    char path[192];
    const int written = snprintf(
        path, sizeof(path), "%s/%s/sessions/%s/occupancy-v1.bin",
        STORAGE_MANAGER_OBJECTS_PATH, object_id, session_id
    );
    if (written <= 0 || (size_t)written >= sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    const esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        return err;
    }
    FILE *file = fopen(path, "rb");
    const bool found = file != NULL;
    if (found) {
        fclose(file);
    }
    storage_manager_operation_end();
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}


static void localization_calculate_set_scan_provenance(
    localization_run_record_t *record,
    bool constraint_applied,
    const scan_occupancy_t *occupancy
)
{
    if (record == NULL) {
        return;
    }
    record->scan_provenance_present = true;
    record->scan_constraint_applied = false;
    record->scan_frame_id[0] = '\0';
    record->occupancy_created_at_ms = 0U;
    record->registration_geometry_computed_at_ms = 0U;
    if (!constraint_applied || occupancy == NULL) {
        return;
    }

    const size_t frame_id_length = strnlen(occupancy->frame_id, sizeof(occupancy->frame_id));
    if (frame_id_length == 0U || frame_id_length >= sizeof(record->scan_frame_id)) {
        return;
    }
    memcpy(record->scan_frame_id, occupancy->frame_id, frame_id_length + 1U);
    record->scan_constraint_applied = true;
    record->occupancy_created_at_ms = occupancy->created_at_ms;
    record->registration_geometry_computed_at_ms =
        occupancy->registration.geometry_computed_at_ms;
}


static bool record_has_rejection_class(const localization_run_record_t *record)
{
    return record != NULL &&
        (record->solved || record->rejection_reason[0] == '\0' ||
         record->failure_class != LOCALIZATION_FAILURE_CLASS_NONE);
}


static esp_err_t localization_calculate_write_picks_retry(
    const char *object_id,
    const char *session_id,
    const localization_pick_record_t *record
)
{
    esp_err_t err = localization_run_store_write_picks(object_id, session_id, record);
    if (err != ESP_OK) {
        err = localization_run_store_write_picks(object_id, session_id, record);
    }
    return err;
}


static int64_t monotonic_us(void)
{
    return esp_timer_get_time();
}


static void free_channel_buffers(
    float *channel_samples[OBJECT_STORE_POD_COUNT]
)
{
    for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        if (channel_samples[i] != NULL) {
            heap_caps_free(channel_samples[i]);
            channel_samples[i] = NULL;
        }
    }
}


static void localization_calculate_persist_memory_failure(
    const char *object_id,
    const char *session_id,
    const localization_calculate_config_t *config,
    const object_store_geometry_t *geometry,
    uint32_t channel_mask,
    localization_dimensionality_t dimensionality,
    uint64_t computed_at_ms,
    localization_calculate_summary_t *summary
)
{
    localization_run_record_t record = {0};
    record.schema_version = LOCALIZATION_RUN_SCHEMA_VERSION;
    record.scan_provenance_present = true;
    snprintf(record.object_id, sizeof(record.object_id), "%s", object_id);
    snprintf(record.session_id, sizeof(record.session_id), "%s", session_id);
    record.computed_at_ms = computed_at_ms;
    record.kind = LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD;
    snprintf(record.method_name, sizeof(record.method_name), "localization_analysis");
    record.method_version = LOCALIZATION_CALCULATE_METHOD_VERSION;
    record.channel_mask_rejected = channel_mask;
    record.dimensionality = dimensionality;
    record.geometry_computed_at_ms =
        localization_calculate_geometry_computed_at_ms(geometry);
    snprintf(
        record.propagation_model_name, sizeof(record.propagation_model_name),
        "%s", config->propagation_model_name
    );
    snprintf(record.material, sizeof(record.material), "%s", config->material);
    record.propagation_speed_mps = config->propagation_speed_mps;
    record.propagation_speed_uncertainty_mps =
        config->propagation_speed_uncertainty_mps;
    record.failure_class = LOCALIZATION_FAILURE_CLASS_INTERNAL;
    snprintf(
        record.rejection_reason, sizeof(record.rejection_reason),
        "insufficient_memory"
    );

    summary->methods_total = 1U;
    summary->methods_completed = 1U;
    summary->methods_attempted = 1U;
    if (localization_run_store_write(object_id, session_id, &record) == ESP_OK) {
        summary->methods_persisted = 1U;
    }
}


esp_err_t localization_calculate_run(
    const char *object_id,
    const char *session_id,
    const localization_calculate_config_t *config,
    uint64_t computed_at_ms,
    localization_calculate_summary_t *summary_out
)
{
    if (
        object_id == NULL ||
        session_id == NULL ||
        config == NULL ||
        summary_out == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(summary_out, 0, sizeof(*summary_out));
    summary_out->tdcas_velocity_status = LOCALIZATION_VELOCITY_STATUS_NOT_APPLICABLE;
    summary_out->psafuvs_velocity_status = LOCALIZATION_VELOCITY_STATUS_NOT_APPLICABLE;
    summary_out->suea_velocity_status = LOCALIZATION_VELOCITY_STATUS_NOT_APPLICABLE;
    summary_out->tdcas_scan_velocity_status = LOCALIZATION_VELOCITY_STATUS_NOT_APPLICABLE;
    summary_out->psafuvs_scan_velocity_status = LOCALIZATION_VELOCITY_STATUS_NOT_APPLICABLE;
    summary_out->suea_scan_velocity_status = LOCALIZATION_VELOCITY_STATUS_NOT_APPLICABLE;

    if (
        !isfinite(config->propagation_speed_mps) ||
        !isfinite(config->propagation_speed_uncertainty_mps) ||
        !isfinite(config->velocity_range.bounds.v_min_mps) ||
        !isfinite(config->velocity_range.bounds.v_max_mps) ||
        !isfinite(config->velocity_range.bounds.vp_vs_min) ||
        !isfinite(config->velocity_range.bounds.vp_vs_max) ||
        !(config->propagation_speed_mps > 0.0) ||
        !(config->propagation_speed_uncertainty_mps >= 0.0) ||
        (config->velocity_range.bounds.set &&
            localization_velocity_bounds_validate(&config->velocity_range.bounds) !=
                LOCALIZATION_VELOCITY_BOUNDS_VALID) ||
        !(config->timing_uncertainty_us > 0.0) ||
        config->semblance_window_half_span_samples == 0U ||
        strnlen(config->material, sizeof(config->material)) >= sizeof(config->material) ||
        !isfinite(config->max_horizontal_uncertainty_m) ||
        config->max_horizontal_uncertainty_m < 0.0
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    object_store_geometry_t geometry;
    memset(&geometry, 0, sizeof(geometry));
    esp_err_t err = seismic_recorder_read_session_geometry(object_id, session_id, &geometry);
    if (err != ESP_OK) {
        return err;
    }

    uint32_t all_channel_mask = 0U;
    const size_t participating_count =
        localization_calculate_participating_channels(&geometry, &all_channel_mask);
    summary_out->participating_channel_count = participating_count;

    const localization_dimensionality_t dimensionality =
        localization_dimensionality_classify(participating_count);
    if (dimensionality == LOCALIZATION_DIMENSIONALITY_INSUFFICIENT) {
        return ESP_ERR_INVALID_STATE;
    }

    /* --- Read a bounded full-resolution prefix of this session ----------- */
    seismic_recorder_session_summary_t session_summary;
    err = seismic_recorder_get_session_summary(object_id, session_id, &session_summary);
    if (err != ESP_OK) {
        return err;
    }
    if (
        (!session_summary.readable && !session_summary.complete) ||
        session_summary.samples_written == 0U ||
        session_summary.sample_rate_hz == 0U
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    localization_calculate_config_t session_config = *config;
    char rate_error[96];
    err = localization_calculate_rescale_sample_windows(
        &session_config,
        session_summary.sample_rate_hz,
        rate_error,
        sizeof(rate_error)
    );
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "session-rate window rescaling failed: %s", rate_error);
        return err;
    }
    config = &session_config;

    const size_t bytes_per_frame =
        sizeof(seismic_frame_t) + SEISMIC_CHANNEL_COUNT * sizeof(float);
    const size_t largest_free_psram_block =
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    bool analysis_limited_by_free_memory = false;
    const size_t max_window_frames = localization_calculate_analysis_window_frames(
        session_summary.sample_rate_hz,
        CONFIG_LOCALIZATION_MAX_ANALYSIS_S,
        (uint64_t)CONFIG_LOCALIZATION_ANALYSIS_PSRAM_BUDGET_KB * 1024ULL,
        largest_free_psram_block,
        LOCALIZATION_ANALYSIS_PSRAM_SAFETY_RESERVE_BYTES,
        bytes_per_frame,
        &analysis_limited_by_free_memory
    );
    const size_t minimum_useful_frames = session_summary.sample_rate_hz;
    if (max_window_frames < minimum_useful_frames) {
        ESP_LOGE(
            TAG,
            "insufficient_memory: largest PSRAM block=%u bytes; need a %u-frame window",
            (unsigned)largest_free_psram_block,
            (unsigned)minimum_useful_frames
        );
        localization_calculate_persist_memory_failure(
            object_id, session_id, config, &geometry, all_channel_mask,
            dimensionality, computed_at_ms, summary_out
        );
        return ESP_ERR_NO_MEM;
    }
    const size_t frame_capacity = session_summary.samples_written < (uint64_t)max_window_frames
        ? (size_t)session_summary.samples_written
        : max_window_frames;
    if (frame_capacity == 0U) {
        return ESP_ERR_INVALID_SIZE;
    }

    seismic_frame_t *frames = heap_caps_malloc(
        sizeof(*frames) * frame_capacity,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (frames == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t frame_count = 0U;
    err = seismic_recorder_read_session_range(
        object_id, session_id, 0U, frame_capacity, frames, NULL, NULL,
        &frame_count, NULL
    );
    if (err != ESP_OK) {
        heap_caps_free(frames);
        return err;
    }
    if (frame_count == 0U || session_summary.sample_rate_hz == 0U) {
        heap_caps_free(frames);
        return ESP_ERR_INVALID_STATE;
    }
    const bool analysis_window_truncated =
        session_summary.samples_written > (uint64_t)frame_count;
    const bool analysis_window_memory_limited =
        analysis_limited_by_free_memory &&
        session_summary.samples_written > (uint64_t)frame_count;

    /* --- Per-channel DC removal, energy, and arrival picking ------------- */

    float *channel_samples[OBJECT_STORE_POD_COUNT] = { 0 };
    localization_event_channel_input_t event_channels[OBJECT_STORE_POD_COUNT];
    memset(event_channels, 0, sizeof(event_channels));

    for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
        event_channels[ch].sample_rate_hz = session_summary.sample_rate_hz;

        if (!geometry.pods[ch].computed) {
            /* No valid sensor position for this channel: it was never a
             * candidate to pick from at all, so it is excluded exactly like
             * any other channel with no usable trigger. */
            event_channels[ch].pick.status = GEOPHONE_PICK_REJECTED_NO_TRIGGER;
            continue;
        }

        /* Defensive default: GEOPHONE_PICK_ACCEPTED is enum value 0, so an
         * untouched pick must never be left at its zeroed state below --
         * that would silently read back as an accepted, zero-arrival-time
         * pick instead of "not yet computed". */
        event_channels[ch].pick.status = GEOPHONE_PICK_REJECTED_NO_TRIGGER;
        event_channels[ch].sensor_position =
            localization_calculate_sensor_position(&geometry.pods[ch]);

        float *samples = heap_caps_malloc(
            sizeof(float) * frame_count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
        if (samples == NULL) {
            free_channel_buffers(channel_samples);
            heap_caps_free(frames);
            return ESP_ERR_NO_MEM;
        }
        channel_samples[ch] = samples;

        for (size_t i = 0U; i < frame_count; ++i) {
            samples[i] = frames[i].channel_uv[ch];
        }

        const seismic_dsp_config_t dsp_config = {
            .sample_rate_hz = session_summary.sample_rate_hz,
            .requested_stages = SEISMIC_DSP_STAGE_DC_REMOVAL,
        };
        /* Both dsp calls only fail for a malformed config or NULL result,
         * neither possible with the fixed, parameter-free stages requested
         * here; still checked and failed closed rather than assumed, since
         * a failed DC removal must never be treated as a validated pick
         * candidate below. */
        err = seismic_dsp_init(&dsp_config);
        seismic_dsp_result_t dsp_result;
        if (err == ESP_OK) {
            err = seismic_dsp_process_block(samples, frame_count, false, &dsp_result);
        }
        if (err != ESP_OK) {
            free_channel_buffers(channel_samples);
            heap_caps_free(frames);
            return err;
        }

        geophone_event_pick_channel_arrival(
            samples, frame_count, session_summary.sample_rate_hz,
            &config->pick_config, &event_channels[ch].pick
        );
    }

    /* The raw frame buffer has now been de-interleaved into per-channel
     * DC-removed windows; nothing below needs the interleaved copy. */
    heap_caps_free(frames);
    frames = NULL;

    /* --- MAX-209: cross-channel physical consistency + xcorr refinement --- */

    const uint32_t sample_rate_hz = session_summary.sample_rate_hz;
    size_t max_lag_samples = 0U;
    double max_sensor_distance_m = 0.0;
    for (size_t a = 0U; a < OBJECT_STORE_POD_COUNT; ++a) {
        for (size_t b = a + 1U; geometry.pods[a].computed && b < OBJECT_STORE_POD_COUNT; ++b) {
            if (!geometry.pods[b].computed) {
                continue;
            }
            const double dx = event_channels[a].sensor_position.x_m - event_channels[b].sensor_position.x_m;
            const double dy = event_channels[a].sensor_position.y_m - event_channels[b].sensor_position.y_m;
            const double dz = event_channels[a].sensor_position.z_m - event_channels[b].sensor_position.z_m;
            const double distance = sqrt(dx * dx + dy * dy + dz * dz);
            if (distance > max_sensor_distance_m) {
                max_sensor_distance_m = distance;
            }
        }
    }

    {
        double arrival_s[OBJECT_STORE_POD_COUNT] = { 0 };
        localization_point3d_t gate_positions[OBJECT_STORE_POD_COUNT];
        uint32_t accepted_mask = 0U;
        for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
            gate_positions[ch] = event_channels[ch].sensor_position;
            if (event_channels[ch].pick.status == GEOPHONE_PICK_ACCEPTED) {
                arrival_s[ch] =
                    localization_calculate_pick_sample_position(&event_channels[ch].pick) /
                    (double)sample_rate_hz;
                accepted_mask |= (1U << ch);
            }
        }
        /* 3 sigma of pick noise plus one sample of quantization. */
        const double tolerance_s =
            3.0 * config->timing_uncertainty_us / 1.0e6 + 1.0 / (double)sample_rate_hz;
        /* MAX-301 S3: the gate's slowest speed comes from the range (see policy.h). */
        const uint32_t consistent_mask = localization_calculate_consistent_pick_mask(
            arrival_s, gate_positions, OBJECT_STORE_POD_COUNT, accepted_mask,
            localization_calculate_gate_min_speed_mps(config), 0.0,
            tolerance_s
        );
        for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
            if ((accepted_mask & (1U << ch)) != 0U && (consistent_mask & (1U << ch)) == 0U) {
                event_channels[ch].pick.status = GEOPHONE_PICK_REJECTED_INCONSISTENT;
            }
        }

        /* Refine every other accepted pick's delay behind the earliest one
         * by cross-correlation (sub-sample); kept only when the waveforms
         * are similar and agree with the independent picks. */
        size_t reference_channel = OBJECT_STORE_POD_COUNT;
        for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
            if (
                event_channels[ch].pick.status == GEOPHONE_PICK_ACCEPTED &&
                (reference_channel == OBJECT_STORE_POD_COUNT || arrival_s[ch] < arrival_s[reference_channel])
            ) {
                reference_channel = ch;
            }
        }
        const double v_min =
            config->propagation_speed_uncertainty_mps > 0.0 &&
            config->propagation_speed_mps > config->propagation_speed_uncertainty_mps
            ? config->propagation_speed_mps - config->propagation_speed_uncertainty_mps
            : config->propagation_speed_mps;
        max_lag_samples =
            (size_t)ceil(max_sensor_distance_m / v_min * (double)sample_rate_hz) + 2U;

        for (size_t ch = 0U; reference_channel < OBJECT_STORE_POD_COUNT && ch < OBJECT_STORE_POD_COUNT; ++ch) {
            if (ch == reference_channel || event_channels[ch].pick.status != GEOPHONE_PICK_ACCEPTED) {
                continue;
            }
            const double reference_position =
                localization_calculate_pick_sample_position(&event_channels[reference_channel].pick);
            const double pick_delay =
                localization_calculate_pick_sample_position(&event_channels[ch].pick) - reference_position;
            double refined_delay = 0.0;
            if (
                localization_calculate_xcorr_relative_delay(
                    channel_samples[reference_channel], channel_samples[ch], frame_count,
                    event_channels[reference_channel].pick.arrival_sample_index,
                    pick_delay, config->pick_config.snr_signal_window_samples,
                    max_lag_samples, LOCALIZATION_CALCULATE_XCORR_MAX_ADJUST_SAMPLES,
                    &refined_delay
                ) &&
                reference_position + refined_delay >= 0.0
            ) {
                const double position = reference_position + refined_delay;
                const double index = ceil(position);
                event_channels[ch].pick.arrival_sample_index = (size_t)index;
                event_channels[ch].pick.arrival_subsample_offset = (float)(position - index);
                event_channels[ch].pick.arrival_time_s = (float)(position / (double)sample_rate_hz);
            }
        }
    }

    bool any_accepted = false;
    size_t reference_sample_index = 0U;
    for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
        if (event_channels[ch].pick.status == GEOPHONE_PICK_ACCEPTED) {
            if (!any_accepted || event_channels[ch].pick.arrival_sample_index < reference_sample_index) {
                reference_sample_index = event_channels[ch].pick.arrival_sample_index;
            }
            any_accepted = true;
        }
    }

    localization_calculate_workspace_t *workspace = heap_caps_calloc(
        1U, sizeof(*workspace), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (workspace == NULL) {
        free_channel_buffers(channel_samples);
        return ESP_ERR_NO_MEM;
    }

    /*
     * MAX-148 (slice 3): persist the per-channel pick evidence this run
     * actually used, versioned by the same computed_at_ms as the method
     * records below -- so a WebUI can explain *why* a channel was rejected
     * (no trigger / no refinement / low SNR) rather than only seeing the
     * aggregate channel_mask_rejected each run record carries. Retry one
     * failed write and report any persistent error after method records and
     * joint fusion have been saved.
     */
    localization_pick_record_t *pick_record = &workspace->pick_record;
    snprintf(pick_record->object_id, sizeof(pick_record->object_id), "%s", object_id);
    snprintf(pick_record->session_id, sizeof(pick_record->session_id), "%s", session_id);
    pick_record->computed_at_ms = computed_at_ms;
    for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
        pick_record->channels[ch].participating = geometry.pods[ch].computed;
        pick_record->channels[ch].pick = event_channels[ch].pick;
    }
    const esp_err_t pick_write_err = localization_calculate_write_picks_retry(
        object_id, session_id, pick_record
    );

    localization_point3d_t participant_positions[OBJECT_STORE_POD_COUNT];
    size_t participant_position_count = 0U;
    for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
        if (geometry.pods[ch].computed) {
            workspace->participant_channel[participant_position_count] = ch;
            participant_positions[participant_position_count++] = event_channels[ch].sensor_position;
        }
    }

    localization_semblance_search_bounds_t semblance_bounds;
    localization_traveltime_search_bounds_t traveltime_bounds;
    localization_calculate_search_bounds(
        participant_positions, participant_position_count, dimensionality,
        config->max_source_depth_m, &semblance_bounds, &traveltime_bounds
    );

    const uint64_t geometry_computed_at_ms =
        localization_calculate_geometry_computed_at_ms(&geometry);

    localization_calculate_run_context_t context = {
        .object_id = object_id,
        .session_id = session_id,
        .computed_at_ms = computed_at_ms,
        .geometry_computed_at_ms = geometry_computed_at_ms,
        .dimensionality = dimensionality,
        .propagation_model_name = config->propagation_model_name,
        .material = config->material,
        .propagation_speed_mps = config->propagation_speed_mps,
        .propagation_speed_uncertainty_mps = config->propagation_speed_uncertainty_mps,
        .velocity_range = &config->velocity_range,
    };

    /* Twelve unconstrained methods (fused below) plus up to eight
     * scan-constrained variants (MAX-147), which are persisted for
     * comparison but never fused -- the joint result stays the
     * unconstrained one. */
    localization_run_record_t *records = workspace->records;
    size_t record_count = 0U;

    /* Probe only for the scan branch before methods start; the full artifact
     * is still loaded at its existing point in the analysis sequence. */
    const esp_err_t occupancy_probe_err = session_occupancy_probe(object_id, session_id);
    const bool scan_variants_planned = occupancy_probe_err != ESP_ERR_NOT_FOUND;
    summary_out->methods_total = LOCALIZATION_CALCULATE_BASE_METHODS +
        (scan_variants_planned ? LOCALIZATION_CALCULATE_SCAN_METHODS : 0U) +
        LOCALIZATION_CALCULATE_FUSION_METHODS;

    /* Every method is timed; the skippable ones stop starting once the
     * caller's deadline has passed (their records then say so). */
    localization_calculate_budget_t budget;
    localization_calculate_budget_init(
        &budget, monotonic_us, config->deadline_us, config->progress, config->progress_context, summary_out,
        config->max_horizontal_uncertainty_m > 0.0
            ? config->max_horizontal_uncertainty_m
            : LOCALIZATION_CALCULATE_DEFAULT_MAX_HORIZONTAL_UNCERTAINTY_M
    );
    bool method_allowed = true;

    /* --- TDOA (Gauss-Newton), via the existing MAX-135 bridge ------------ */

    (void)localization_calculate_budget_begin(&budget, "tdoa_gauss_newton", false);
    localization_config_t tdoa_config = localization_default_config();
    tdoa_config.propagation_speed_mps = config->propagation_speed_mps;

    localization_event_result_t event_result;
    const esp_err_t tdoa_err = localization_event_solve(
        event_channels, OBJECT_STORE_POD_COUNT, &tdoa_config, NULL, &event_result
    );
    summary_out->methods_attempted++;

    localization_event_result_to_run_record(
        &event_result, object_id, session_id, computed_at_ms, geometry_computed_at_ms,
        LOCALIZATION_CALCULATE_METHOD_VERSION, config->propagation_model_name, config->material,
        config->propagation_speed_mps, config->propagation_speed_uncertainty_mps,
        config->timing_uncertainty_us, &records[record_count]
    );
    if (tdoa_err != ESP_OK) {
        localization_calculate_set_rejection(
            &records[record_count], "TDOA event bridge call failed", LOCALIZATION_FAILURE_CLASS_INTERNAL
        );
    }
    /* MAX-212: the Gauss-Newton TDOA solve is always 3D. */
    localization_calculate_apply_redundancy(
        &records[record_count],
        localization_calculate_degrees_of_freedom(event_result.channels_used_count, false)
    );
    localization_calculate_budget_end(&budget, &records[record_count]);
    ++record_count;

    /* --- Statistical traveltime inversion (MAX-136) ---------------------- */

    localization_traveltime_pick_t traveltime_picks[OBJECT_STORE_POD_COUNT];
    uint32_t traveltime_used_mask = 0U;
    uint32_t traveltime_rejected_mask = 0U;
    for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
        const bool usable =
            geometry.pods[ch].computed &&
            event_channels[ch].pick.status == GEOPHONE_PICK_ACCEPTED;

        traveltime_picks[ch].observation.sensor_position = event_channels[ch].sensor_position;
        traveltime_picks[ch].observation.arrival_time_us = usable
            ? localization_calculate_arrival_time_us_at(
                  localization_calculate_pick_sample_position(&event_channels[ch].pick),
                  session_summary.sample_rate_hz
              )
            : 0;
        traveltime_picks[ch].weight = usable ? 1.0 : 0.0;

        if (usable) {
            traveltime_used_mask |= (1U << ch);
        } else {
            traveltime_rejected_mask |= (1U << ch);
        }
    }

    const localization_traveltime_config_t traveltime_config = {
        .propagation_speed_mps = config->propagation_speed_mps,
        .timing_uncertainty_us = config->timing_uncertainty_us,
        .refine_divisor = LOCALIZATION_CALCULATE_REFINE_DIVISOR,
        /* MAX-208: streaming search within the default candidate budget; the
         * per-candidate surface is not persisted, so it is never built. */
        .max_coarse_candidates = 0U,
        .retain_surface = false,
    };
    localization_traveltime_result_t traveltime_result;
    memset(&traveltime_result, 0, sizeof(traveltime_result));
    esp_err_t traveltime_err = ESP_ERR_INVALID_STATE;
    if (localization_calculate_budget_begin(&budget, "traveltime_inversion", true)) {
        traveltime_err = localization_traveltime_search(
            traveltime_picks, OBJECT_STORE_POD_COUNT, &traveltime_bounds,
            &traveltime_config, &traveltime_result
        );
        summary_out->methods_attempted++;
    }

    localization_calculate_traveltime_to_run_record(
        &context, traveltime_err == ESP_OK ? &traveltime_result : NULL,
        traveltime_used_mask, traveltime_rejected_mask,
        LOCALIZATION_CALCULATE_METHOD_VERSION, config->timing_uncertainty_us,
        &records[record_count]
    );
    localization_calculate_apply_redundancy(
        &records[record_count],
        localization_calculate_degrees_of_freedom(
            traveltime_result.picks_used_count,
            dimensionality == LOCALIZATION_DIMENSIONALITY_PLANAR_2D
        )
    );
    localization_calculate_budget_end(&budget, &records[record_count]);
    ++record_count;
    free(traveltime_result.surface);

    /* --- Semblance / coherency (MAX-137) ---------------------------------
     * Uses every participating channel's raw DC-removed window regardless
     * of that channel's own pick outcome (a coherency operator measures the
     * waveform shape directly), but can only run at all once some channel,
     * anywhere, has an accepted pick to anchor its search window on. */

    const bool semblance_attempted = any_accepted;
    localization_semblance_result_t semblance_result;
    memset(&semblance_result, 0, sizeof(semblance_result));
    esp_err_t semblance_err = ESP_ERR_INVALID_STATE;

    method_allowed = localization_calculate_budget_begin(&budget, "semblance", true);
    if (semblance_attempted && method_allowed) {
        localization_semblance_channel_t semblance_channels[OBJECT_STORE_POD_COUNT];
        size_t semblance_channel_count = 0U;
        for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
            if (geometry.pods[ch].computed) {
                semblance_channels[semblance_channel_count].samples = channel_samples[ch];
                semblance_channels[semblance_channel_count].sample_count = frame_count;
                semblance_channels[semblance_channel_count].sample_rate_hz = session_summary.sample_rate_hz;
                semblance_channels[semblance_channel_count].sensor_position = event_channels[ch].sensor_position;
                ++semblance_channel_count;
            }
        }

        const localization_semblance_config_t semblance_config = {
            .propagation_speed_mps = config->propagation_speed_mps,
            .reference_sample_index = reference_sample_index,
            .window_half_span_samples = config->semblance_window_half_span_samples,
            .refine_divisor = LOCALIZATION_CALCULATE_REFINE_DIVISOR,
        };
        semblance_err = localization_semblance_search(
            semblance_channels, semblance_channel_count, &semblance_bounds,
            &semblance_config, &semblance_result
        );
        summary_out->methods_attempted++;
    }

    localization_calculate_semblance_to_run_record(
        &context, (semblance_attempted && semblance_err == ESP_OK) ? &semblance_result : NULL,
        semblance_attempted
            ? "semblance search call failed"
            : "no accepted arrival pick to anchor the search window",
        semblance_attempted
            ? LOCALIZATION_FAILURE_CLASS_INTERNAL
            : LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN,
        all_channel_mask, (~all_channel_mask) & LOCALIZATION_CALCULATE_ALL_CHANNELS_MASK,
        LOCALIZATION_CALCULATE_METHOD_VERSION, &records[record_count]
    );
    localization_calculate_budget_end(&budget, &records[record_count]);
    ++record_count;

    /* --- Energy baseline (MAX-138 part 1) --------------------------------- */

    /* MAX-212: energy over the EVENT window (earliest accepted pick +
     * signal window + the array's largest moveout), not the whole recording,
     * which is dominated by background noise. Without an accepted pick there
     * is no event window, so the baseline is not attempted. */
    (void)localization_calculate_budget_begin(&budget, "energy_baseline", false);
    size_t energy_start = 0U;
    size_t energy_end = 0U;
    const bool energy_window_known =
        any_accepted &&
        localization_calculate_event_window(
            reference_sample_index, frame_count,
            config->pick_config.snr_signal_window_samples,
            max_sensor_distance_m, config->propagation_speed_mps,
            session_summary.sample_rate_hz, &energy_start, &energy_end
        );

    localization_energy_baseline_channel_t energy_channels[OBJECT_STORE_POD_COUNT];
    size_t energy_channel_count = 0U;
    for (size_t ch = 0U; energy_window_known && ch < OBJECT_STORE_POD_COUNT; ++ch) {
        if (geometry.pods[ch].computed && channel_samples[ch] != NULL) {
            double sum_squares = 0.0;
            for (size_t i = energy_start; i < energy_end; ++i) {
                sum_squares += (double)channel_samples[ch][i] * (double)channel_samples[ch][i];
            }
            energy_channels[energy_channel_count].sensor_position = event_channels[ch].sensor_position;
            energy_channels[energy_channel_count].energy =
                energy_end > energy_start ? sum_squares / (double)(energy_end - energy_start) : 0.0;
            energy_channels[energy_channel_count].weight = 1.0;
            ++energy_channel_count;
        }
    }

    localization_energy_baseline_result_t energy_result;
    memset(&energy_result, 0, sizeof(energy_result));
    esp_err_t energy_err = ESP_ERR_INVALID_STATE;
    if (energy_window_known) {
        energy_err = localization_energy_baseline_estimate(
            energy_channels, energy_channel_count, &energy_result
        );
        summary_out->methods_attempted++;
    }

    localization_calculate_energy_baseline_to_run_record(
        &context, energy_err == ESP_OK ? &energy_result : NULL,
        all_channel_mask, (~all_channel_mask) & LOCALIZATION_CALCULATE_ALL_CHANNELS_MASK,
        LOCALIZATION_CALCULATE_METHOD_VERSION, &records[record_count]
    );
    if (!energy_window_known) {
        localization_calculate_set_rejection(
            &records[record_count], "no accepted arrival pick to define the event energy window",
            LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN
        );
    } else if (energy_err != ESP_OK) {
        localization_calculate_set_rejection(
            &records[record_count], "energy baseline call failed", LOCALIZATION_FAILURE_CLASS_INTERNAL
        );
    }
    localization_calculate_budget_end(&budget, &records[record_count]);
    ++record_count;

    /* --- Velocity-free methods (TDCAS, PSAFUVS, SUEA) ----------------------
     * Same accepted P picks as TDOA / traveltime inversion, but the event's
     * own velocity is estimated instead of trusted. */

    localization_observation_t *p_observations = workspace->p_observations;
    size_t p_observation_count = 0U;
    for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
        if ((traveltime_used_mask & (1U << ch)) != 0U) {
            workspace->p_channel[p_observation_count] = ch;
            p_observations[p_observation_count++] = traveltime_picks[ch].observation;
        }
    }

    /* TDCAS: needs 6 picks (5 on a planar array); with fewer the record
     * says so rather than inventing a position. */
    const localization_tdcas_config_t tdcas_config = {
        .velocity_bounds = &config->velocity_range.bounds,
        .timing_uncertainty_us = config->timing_uncertainty_us,
    };
    localization_tdcas_result_t *tdcas_result = &workspace->tdcas;
    esp_err_t tdcas_err = ESP_ERR_INVALID_ARG;
    if (localization_calculate_budget_begin(&budget, "tdcas", true) && p_observation_count > 0U) {
        tdcas_err = localization_tdcas_solve(p_observations, p_observation_count, &tdcas_config, tdcas_result);
        summary_out->methods_attempted++;
    }
    localization_calculate_tdcas_to_run_record(
        &context, tdcas_err == ESP_OK ? tdcas_result : NULL,
        traveltime_used_mask, traveltime_rejected_mask,
        LOCALIZATION_CALCULATE_METHOD_VERSION, config->timing_uncertainty_us,
        &records[record_count], &summary_out->tdcas_velocity_status
    );
    if (p_observation_count > 0U && tdcas_err != ESP_OK) {
        localization_calculate_set_rejection(
            &records[record_count], "TDCAS solver call failed", LOCALIZATION_FAILURE_CLASS_INTERNAL
        );
    }
    localization_calculate_apply_redundancy(
        &records[record_count],
        localization_calculate_velocity_free_degrees_of_freedom(
            p_observation_count, LOCALIZATION_CALCULATE_TDCAS_UNKNOWNS
        )
    );
    localization_calculate_budget_end(&budget, &records[record_count]);
    ++record_count;

    /* PSAFUVS: an S pick after every accepted P pick, when configured. */
    localization_psafuvs_observation_t *ps_observations = workspace->ps_observations;
    size_t ps_observation_count = 0U;
    uint32_t ps_used_mask = 0U;
    const char *psafuvs_not_attempted = "S-wave picking not configured: PSAFUVS needs P and S arrivals";
    method_allowed = localization_calculate_budget_begin(&budget, "psafuvs", true);
    for (size_t ch = 0U; method_allowed && config->s_pick_enabled && ch < OBJECT_STORE_POD_COUNT; ++ch) {
        if ((traveltime_used_mask & (1U << ch)) == 0U || channel_samples[ch] == NULL) {
            continue;
        }
        geophone_channel_pick_t s_pick;
        if (!geophone_event_pick_secondary_arrival(
                channel_samples[ch], frame_count, session_summary.sample_rate_hz,
                event_channels[ch].pick.arrival_sample_index, &config->s_pick_config, &s_pick
            )) {
            continue;
        }
        const int64_t s_arrival_us = localization_calculate_arrival_time_us(
            s_pick.arrival_sample_index, session_summary.sample_rate_hz
        );
        if (s_arrival_us <= traveltime_picks[ch].observation.arrival_time_us) {
            continue;
        }
        workspace->s_valid[ch] = true;
        workspace->s_arrival_us[ch] = s_arrival_us;
        workspace->s_snr[ch] = s_pick.snr;
        ps_observations[ps_observation_count].sensor_position = event_channels[ch].sensor_position;
        ps_observations[ps_observation_count].p_arrival_time_us = traveltime_picks[ch].observation.arrival_time_us;
        ps_observations[ps_observation_count].s_arrival_time_us = s_arrival_us;
        workspace->ps_channel[ps_observation_count] = ch;
        ps_used_mask |= 1U << ch;
        ++ps_observation_count;
    }
    if (config->s_pick_enabled) {
        psafuvs_not_attempted = "no channel has an accepted S pick after its P pick";
    }
    localization_psafuvs_config_t psafuvs_config = localization_psafuvs_default_config();
    psafuvs_config.velocity_bounds = &config->velocity_range.bounds;
    psafuvs_config.timing_uncertainty_us = config->timing_uncertainty_us;
    localization_psafuvs_result_t *psafuvs_result = &workspace->psafuvs;
    esp_err_t psafuvs_err = ESP_ERR_INVALID_STATE;
    if (ps_observation_count > 0U) {
        psafuvs_err = localization_psafuvs_solve(
            ps_observations, ps_observation_count, &psafuvs_config, psafuvs_result
        );
        summary_out->methods_attempted++;
    }
    localization_calculate_psafuvs_to_run_record(
        &context, psafuvs_err == ESP_OK ? psafuvs_result : NULL, psafuvs_not_attempted,
        LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN,
        ps_used_mask, (~ps_used_mask) & LOCALIZATION_CALCULATE_ALL_CHANNELS_MASK,
        LOCALIZATION_CALCULATE_METHOD_VERSION, config->timing_uncertainty_us,
        &records[record_count], &summary_out->psafuvs_velocity_status
    );
    if (ps_observation_count > 0U && psafuvs_err != ESP_OK) {
        localization_calculate_set_rejection(
            &records[record_count], "PSAFUVS solver call failed", LOCALIZATION_FAILURE_CLASS_INTERNAL
        );
    }
    localization_calculate_apply_redundancy(
        &records[record_count],
        localization_calculate_velocity_free_degrees_of_freedom(
            2U * ps_observation_count, LOCALIZATION_CALCULATE_PSAFUVS_UNKNOWNS
        )
    );
    localization_calculate_budget_end(&budget, &records[record_count]);
    ++record_count;

    /* SUEA without a scan: every cell passable (straight rays); the
     * caller's velocity enters only as a prior with its own uncertainty (a
     * 0 "unknown" uncertainty fixes it, as every other method uses it). */
    localization_point3d_t suea_min;
    localization_point3d_t suea_max;
    localization_calculate_suea_grid_bounds(&traveltime_bounds, &suea_min, &suea_max);
    localization_suea_grid_t *suea_grid = &workspace->suea_grid;
    const esp_err_t suea_grid_err = localization_suea_grid_init(
        suea_grid, &suea_min, &suea_max, traveltime_bounds.step_m, 0U
    );
    localization_suea_pick_t *suea_picks = workspace->suea_picks;
    for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
        suea_picks[ch].sensor_position = event_channels[ch].sensor_position;
        if (dimensionality == LOCALIZATION_DIMENSIONALITY_PLANAR_2D) {
            /* The planar search is one layer at the mean sensor height. */
            suea_picks[ch].sensor_position.z_m = traveltime_bounds.min_corner.z_m;
        }
        suea_picks[ch].arrival_time_us = traveltime_picks[ch].observation.arrival_time_us;
        suea_picks[ch].weight = traveltime_picks[ch].weight;
    }
    const localization_suea_config_t suea_config = {
        .timing_uncertainty_us = config->timing_uncertainty_us,
        .prior_velocity_mps = config->propagation_speed_mps,
        .prior_velocity_sigma_mps = config->propagation_speed_uncertainty_mps,
        .refine_divisor = LOCALIZATION_CALCULATE_REFINE_DIVISOR,
        .velocity_bounds = &config->velocity_range.bounds,
    };
    localization_suea_result_t *suea_result = &workspace->suea;
    esp_err_t suea_err = ESP_ERR_INVALID_STATE;
    if (localization_calculate_budget_begin(&budget, "suea", true)) {
        suea_err = suea_grid_err;
        if (suea_grid_err == ESP_OK) {
            suea_err = localization_suea_locate(
                suea_grid, suea_picks, OBJECT_STORE_POD_COUNT, &suea_config, suea_result
            );
        }
        summary_out->methods_attempted++;
    }
    localization_calculate_suea_to_run_record(
        &context, suea_err == ESP_OK ? suea_result : NULL,
        traveltime_used_mask, traveltime_rejected_mask,
        LOCALIZATION_CALCULATE_METHOD_VERSION, config->timing_uncertainty_us,
        &records[record_count], &summary_out->suea_velocity_status
    );
    if (suea_grid_err != ESP_OK) {
        localization_calculate_set_rejection(
            &records[record_count], "SUEA grid model could not be built", LOCALIZATION_FAILURE_CLASS_INTERNAL
        );
    } else if (p_observation_count > 0U && suea_err != ESP_OK) {
        localization_calculate_set_rejection(
            &records[record_count], "SUEA solver call failed", LOCALIZATION_FAILURE_CLASS_INTERNAL
        );
    }
    localization_calculate_apply_redundancy(
        &records[record_count],
        localization_calculate_degrees_of_freedom(
            suea_result->picks_used_count,
            dimensionality == LOCALIZATION_DIMENSIONALITY_PLANAR_2D
        )
    );
    localization_calculate_budget_end(&budget, &records[record_count]);
    ++record_count;

    /* --- localization_extended ---------------------------------------------
     * Same P arrivals again, used five more ways: waveform pair delays,
     * a robust consensus, P + S-P jointly, eikonal travel times and a
     * posterior region. All share the pick evidence group. */

    const localization_extended_bounds_t extended_bounds =
        localization_calculate_extended_bounds(&traveltime_bounds);
    const bool planar = dimensionality == LOCALIZATION_DIMENSIONALITY_PLANAR_2D;
    const size_t participants = participant_position_count;

    /* Cross-correlation TDOA over every participating pod's waveform. */
    localization_xcorr_tdoa_result_t *xcorr_result = &workspace->xcorr;
    esp_err_t xcorr_err = ESP_ERR_INVALID_STATE;
    for (size_t i = 0U; i < participants; ++i) {
        const size_t ch = workspace->participant_channel[i];
        localization_xcorr_channel_t *channel = &workspace->xcorr_channels[i];
        channel->samples = channel_samples[ch];
        channel->sample_count = frame_count;
        channel->sample_rate_hz = sample_rate_hz;
        channel->sensor_position = event_channels[ch].sensor_position;
        channel->pick_valid = event_channels[ch].pick.status == GEOPHONE_PICK_ACCEPTED;
        channel->pick_sample = channel->pick_valid
            ? localization_calculate_pick_sample_position(&event_channels[ch].pick)
            : 0.0;
    }
    const localization_xcorr_tdoa_config_t xcorr_config = {
        .propagation_speed_mps = config->propagation_speed_mps,
        .timing_uncertainty_us = config->timing_uncertainty_us,
        .window_samples = config->pick_config.snr_signal_window_samples > 4U
            ? config->pick_config.snr_signal_window_samples : 4U,
        .max_lag_samples = max_lag_samples > 0U ? max_lag_samples : 1U,
        .min_correlation = LOCALIZATION_CALCULATE_XCORR_MIN_CORRELATION,
        .max_lattice_points = LOCALIZATION_CALCULATE_SEED_LATTICE_POINTS,
    };
    method_allowed = localization_calculate_budget_begin(&budget, "xcorr_tdoa", true);
    if (participants >= 2U && any_accepted && method_allowed) {
        size_t pair_count = 0U;
        xcorr_err = localization_xcorr_measure_pairs(
            workspace->xcorr_channels, participants, &xcorr_config,
            workspace->xcorr_pairs, &pair_count, NULL
        );
        if (xcorr_err == ESP_OK) {
            xcorr_err = localization_xcorr_tdoa_locate(
                workspace->xcorr_channels, participants, workspace->xcorr_pairs, pair_count,
                &extended_bounds, &xcorr_config, xcorr_result
            );
        }
        summary_out->methods_attempted++;
    }
    const uint32_t xcorr_channels_used = observation_mask_to_channels(
        xcorr_err == ESP_OK ? xcorr_result->channel_mask_used : 0U, workspace->participant_channel, participants
    );
    localization_calculate_xcorr_tdoa_to_run_record(
        &context, xcorr_err == ESP_OK ? xcorr_result : NULL,
        xcorr_channels_used, all_channel_mask & ~xcorr_channels_used,
        LOCALIZATION_CALCULATE_METHOD_VERSION, config->timing_uncertainty_us,
        &records[record_count]
    );
    if (xcorr_err != ESP_OK) {
        if (!any_accepted) {
            localization_calculate_set_rejection(
                &records[record_count], "no accepted arrival pick to anchor the correlation windows",
                LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN
            );
        } else if (participants < 2U) {
            localization_calculate_set_rejection(
                &records[record_count], "fewer than 2 participating sensors for cross-correlation TDOA",
                LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS
            );
        } else {
            localization_calculate_set_rejection(
                &records[record_count], "cross-correlation TDOA input rejected",
                LOCALIZATION_FAILURE_CLASS_INTERNAL
            );
        }
    }
    localization_calculate_apply_redundancy(
        &records[record_count],
        localization_calculate_degrees_of_freedom(xcorr_result->channels_used, planar)
    );
    localization_calculate_budget_end(&budget, &records[record_count]);
    ++record_count;

    /* Robust M-estimator + minimal-subset consensus on the P picks. */
    localization_robust_result_t *robust_result = &workspace->robust;
    esp_err_t robust_err = ESP_ERR_INVALID_STATE;
    if (localization_calculate_budget_begin(&budget, "robust_tdoa", true) && p_observation_count > 0U) {
        localization_robust_config_t robust_config = localization_robust_default_config();
        robust_config.propagation_speed_mps = config->propagation_speed_mps;
        robust_config.timing_uncertainty_us = config->timing_uncertainty_us;
        robust_config.max_lattice_points = LOCALIZATION_CALCULATE_SEED_LATTICE_POINTS;
        robust_err = localization_robust_solve(
            p_observations, p_observation_count, &extended_bounds, &robust_config, robust_result
        );
        summary_out->methods_attempted++;
    }
    const uint32_t robust_outliers = observation_mask_to_channels(
        robust_err == ESP_OK ? robust_result->outlier_mask : 0U, workspace->p_channel, p_observation_count
    );
    localization_calculate_robust_to_run_record(
        &context, robust_err == ESP_OK ? robust_result : NULL,
        traveltime_used_mask & ~robust_outliers, traveltime_rejected_mask | robust_outliers,
        LOCALIZATION_CALCULATE_METHOD_VERSION, config->timing_uncertainty_us,
        &records[record_count]
    );
    if (p_observation_count == 0U) {
        localization_calculate_set_rejection(
            &records[record_count], "no accepted arrival pick for robust fit",
            LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN
        );
    } else if (robust_err != ESP_OK) {
        localization_calculate_set_rejection(
            &records[record_count], "robust fit call failed", LOCALIZATION_FAILURE_CLASS_INTERNAL
        );
    }
    localization_calculate_apply_redundancy(
        &records[record_count], robust_err == ESP_OK ? robust_result->degrees_of_freedom : -1
    );
    localization_calculate_budget_end(&budget, &records[record_count]);
    ++record_count;

    /* PSP: P picks everywhere, S-P where an S pick exists, each S weighted
     * by how clean its onset is relative to the P onset. */
    localization_psp_result_t *psp_result = &workspace->psp;
    esp_err_t psp_err = ESP_ERR_INVALID_STATE;
    size_t psp_s_count = 0U;
    for (size_t i = 0U; i < p_observation_count; ++i) {
        const size_t ch = workspace->p_channel[i];
        localization_psp_observation_t *o = &workspace->psp_observations[i];
        o->sensor_position = p_observations[i].sensor_position;
        o->p_arrival_time_us = p_observations[i].arrival_time_us;
        o->s_valid = workspace->s_valid[ch];
        o->s_arrival_time_us = workspace->s_arrival_us[ch];
        o->s_minus_p_sigma_us = localization_calculate_s_minus_p_sigma_us(
            config->timing_uncertainty_us, event_channels[ch].pick.snr, workspace->s_snr[ch]
        );
        psp_s_count += o->s_valid ? 1U : 0U;
    }
    if (localization_calculate_budget_begin(&budget, "psp_joint", true) && psp_s_count > 0U) {
        const localization_psp_config_t psp_config = {
            .timing_uncertainty_us = config->timing_uncertainty_us,
            .p_velocity_mps = config->propagation_speed_mps,
            .p_velocity_sigma_mps = config->propagation_speed_uncertainty_mps,
            .max_lattice_points = LOCALIZATION_CALCULATE_SEED_LATTICE_POINTS,
        };
        psp_err = localization_psp_solve(
            workspace->psp_observations, p_observation_count, &extended_bounds, &psp_config, psp_result
        );
        summary_out->methods_attempted++;
    }
    localization_calculate_psp_to_run_record(
        &context, psp_err == ESP_OK ? psp_result : NULL, psafuvs_not_attempted,
        LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN,
        traveltime_used_mask, traveltime_rejected_mask,
        LOCALIZATION_CALCULATE_METHOD_VERSION, config->timing_uncertainty_us,
        &records[record_count]
    );
    if (psp_s_count > 0U && psp_err != ESP_OK) {
        localization_calculate_set_rejection(
            &records[record_count], "joint P-TDOA and P-S fit call failed", LOCALIZATION_FAILURE_CLASS_INTERNAL
        );
    }
    if (psp_err == ESP_OK) {
        localization_calculate_apply_redundancy(&records[record_count], psp_result->degrees_of_freedom);
    }
    localization_calculate_budget_end(&budget, &records[record_count]);
    ++record_count;

    /* Participant-indexed picks for the eikonal and Bayesian methods (a pod
     * without an accepted pick has weight 0). */
    for (size_t i = 0U; i < participants; ++i) {
        const size_t ch = workspace->participant_channel[i];
        workspace->eikonal_sensors[i] = event_channels[ch].sensor_position;
        if (planar) {
            workspace->eikonal_sensors[i].z_m = traveltime_bounds.min_corner.z_m;
        }
        workspace->eikonal_picks[i].arrival_time_us = traveltime_picks[ch].observation.arrival_time_us;
        workspace->eikonal_picks[i].weight = traveltime_picks[ch].weight;
        workspace->bayes_picks[i].sensor_position = event_channels[ch].sensor_position;
        workspace->bayes_picks[i].arrival_time_us = traveltime_picks[ch].observation.arrival_time_us;
        workspace->bayes_picks[i].weight = traveltime_picks[ch].weight;
    }

    /* Bayesian posterior, straight rays. Student-t: a wrong pick widens
     * the region instead of moving it. */
    const bool velocity_uncertain = config->propagation_speed_uncertainty_mps > 0.0;
    localization_bayes_config_t bayes_config = {
        .velocity_mps = config->propagation_speed_mps,
        .velocity_sigma_mps = config->propagation_speed_uncertainty_mps,
        .timing_uncertainty_us = config->timing_uncertainty_us,
        .likelihood = LOCALIZATION_BAYES_STUDENT_T,
        .velocity_nodes = LOCALIZATION_CALCULATE_BAYES_VELOCITY_NODES,
        .max_cells = LOCALIZATION_CALCULATE_BAYES_MAX_CELLS,
        .refine_divisor = LOCALIZATION_CALCULATE_REFINE_DIVISOR,
        /* Straight rays through rubble have no known model error: the
         * misfit check widens the region when the data say so. */
        .inflate_on_misfit = true,
    };
    localization_bayes_result_t *bayes_result = &workspace->bayes;
    esp_err_t bayes_err = ESP_ERR_INVALID_STATE;
    if (localization_calculate_budget_begin(&budget, "bayesian", true) && p_observation_count > 0U) {
        bayes_err = localization_bayes_locate(
            workspace->bayes_picks, participants, &extended_bounds, &bayes_config, bayes_result
        );
        summary_out->methods_attempted++;
    }
    localization_calculate_bayes_to_run_record(
        &context, bayes_err == ESP_OK ? bayes_result : NULL, "bayesian",
        "no accepted arrival pick", LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN, velocity_uncertain,
        traveltime_used_mask, traveltime_rejected_mask,
        LOCALIZATION_CALCULATE_METHOD_VERSION, config->timing_uncertainty_us,
        &records[record_count]
    );
    if (p_observation_count > 0U && bayes_err != ESP_OK) {
        localization_calculate_set_rejection(
            &records[record_count], "Bayesian location call failed", LOCALIZATION_FAILURE_CLASS_INTERNAL
        );
    }
    localization_calculate_apply_redundancy(
        &records[record_count], localization_calculate_degrees_of_freedom(bayes_result->picks_used, planar)
    );
    localization_calculate_budget_end(&budget, &records[record_count]);
    ++record_count;

    /* Eikonal travel times in the (homogeneous, without a scan) model over
     * the SUEA box; the scan variant below makes its voids impassable. */
    localization_eikonal_model_t *eikonal_model = &workspace->eikonal_model;
    const esp_err_t eikonal_model_err = localization_eikonal_model_init(
        eikonal_model, &suea_min, &suea_max, traveltime_bounds.step_m,
        LOCALIZATION_CALCULATE_EIKONAL_MAX_NODES, config->propagation_speed_mps
    );
    const localization_eikonal_locate_config_t eikonal_config = {
        .timing_uncertainty_us = config->timing_uncertainty_us,
        .velocity_scale_sigma = config->propagation_speed_uncertainty_mps / config->propagation_speed_mps,
        .refine_divisor = LOCALIZATION_CALCULATE_REFINE_DIVISOR,
    };
    localization_eikonal_result_t *eikonal_result = &workspace->eikonal;
    /*
     * MAX-168 host-test finding: this must NOT default to eikonal_model_err.
     * LOCALIZATION_EIKONAL_SOLVED == 0, the same value workspace->eikonal
     * (heap_caps_calloc'd, never written when the if-block below does not
     * run -- e.g. zero accepted picks) is already zeroed to. Pre-seeding
     * eikonal_err with a successful model_init result let a never-run
     * location fall through as "solved" with a fabricated (0, 0, 0)
     * position whenever the model built fine but there were no picks to
     * locate from. Defaulting to an explicit failure here (like every
     * other method's own not-yet-attempted default) closes that gap; a
     * genuine model_init failure still reaches the record as NULL exactly
     * as before, via the `if` condition's own eikonal_model_err check.
     */
    esp_err_t eikonal_err = ESP_ERR_INVALID_STATE;
    method_allowed = localization_calculate_budget_begin(&budget, "eikonal_fmm", true);
    if (eikonal_model_err == ESP_OK && p_observation_count > 0U && method_allowed) {
        eikonal_err = localization_eikonal_fields_compute(
            eikonal_model, workspace->eikonal_sensors, participants, &workspace->eikonal_fields
        );
        if (eikonal_err == ESP_OK) {
            eikonal_err = localization_eikonal_locate(
                eikonal_model, &workspace->eikonal_fields, workspace->eikonal_picks, participants,
                &eikonal_config, eikonal_result
            );
            localization_eikonal_fields_free(&workspace->eikonal_fields);
        }
        summary_out->methods_attempted++;
    }
    localization_calculate_eikonal_to_run_record(
        &context, eikonal_err == ESP_OK ? eikonal_result : NULL, "eikonal_fmm",
        p_observation_count > 0U ? "eikonal model could not be built" : "no accepted arrival pick",
        p_observation_count > 0U ? LOCALIZATION_FAILURE_CLASS_INTERNAL : LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN,
        velocity_uncertain, traveltime_used_mask, traveltime_rejected_mask,
        LOCALIZATION_CALCULATE_METHOD_VERSION, config->timing_uncertainty_us,
        &records[record_count]
    );
    if (eikonal_model_err == ESP_OK && p_observation_count > 0U && eikonal_err != ESP_OK) {
        localization_calculate_set_rejection(
            &records[record_count], "eikonal location call failed", LOCALIZATION_FAILURE_CLASS_INTERNAL
        );
    }
    localization_calculate_apply_redundancy(
        &records[record_count], localization_calculate_degrees_of_freedom(eikonal_result->picks_used, planar)
    );
    localization_calculate_budget_end(&budget, &records[record_count]);
    ++record_count;

    const size_t fused_record_count = record_count;
    /* Joint fusion consumes only this unconstrained prefix, so its v6 scan
     * provenance remains explicitly false/null/0. */

    /* --- MAX-147: scan-occupancy-constrained variants ---------------------
     * Only from the occupancy saved with THIS session's scan, and only with a
     * registration made against this exact frozen geometry; otherwise the
     * constraint is recorded as not applied (fail closed). The
     * unconstrained records above are always kept for comparison. */
    const size_t scan_record_start = record_count;
    bool scan_constraint_applied[LOCALIZATION_CALCULATE_SCAN_METHODS] = { false };
    scan_occupancy_t occupancy;
    const esp_err_t occupancy_err = scan_variants_planned
        ? load_session_occupancy(object_id, session_id, &occupancy)
        : ESP_ERR_NOT_FOUND;
    if (scan_variants_planned) {
        char current_scan_frame_id[SCAN_OCCUPANCY_FRAME_ID_SIZE] = { 0 };
        const esp_err_t scan_identity_err = occupancy_err == ESP_OK
            ? load_session_scan_frame_id(object_id, session_id, current_scan_frame_id)
            : ESP_ERR_NOT_FOUND;
        const bool scan_identity_matches = occupancy_err == ESP_OK &&
            scan_identity_err == ESP_OK && strcmp(occupancy.frame_id, current_scan_frame_id) == 0;
        const bool usable = scan_identity_matches &&
            scan_occupancy_registration_usable(&occupancy, geometry_computed_at_ms);
        const char *not_applied_reason = occupancy_err != ESP_OK
            ? "scan occupancy artifact unreadable or invalid: constraint not applied"
            : (scan_identity_err != ESP_OK
                ? "current scan identity unavailable (scan_identity_unavailable): constraint not applied"
                : (!scan_identity_matches
                    ? "scan generation mismatch (scan_generation_mismatch): constraint not applied"
                    : "no valid sensor<->scan registration for this session geometry (MAX-145): constraint not applied"));
        const localization_failure_class_t not_applied_failure_class =
            occupancy_err != ESP_OK || scan_identity_err != ESP_OK || !scan_identity_matches
                ? LOCALIZATION_FAILURE_CLASS_INTERNAL
                : LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS;

        /* Traveltime, constrained. */
        localization_traveltime_result_t constrained_traveltime;
        memset(&constrained_traveltime, 0, sizeof(constrained_traveltime));
        esp_err_t constrained_traveltime_err = ESP_ERR_INVALID_STATE;
        scan_constraint_applied[0] =
            localization_calculate_budget_begin(&budget, "traveltime_scan_constrained", true) && usable;
        if (scan_constraint_applied[0]) {
            localization_traveltime_config_t constrained_config = traveltime_config;
            constrained_config.candidate_filter.allowed = localization_calculate_scan_candidate_allowed;
            constrained_config.candidate_filter.context = &occupancy;
            constrained_traveltime_err = localization_traveltime_search(
                traveltime_picks, OBJECT_STORE_POD_COUNT, &traveltime_bounds,
                &constrained_config, &constrained_traveltime
            );
            summary_out->methods_attempted++;
        }
        localization_calculate_traveltime_to_run_record(
            &context, constrained_traveltime_err == ESP_OK ? &constrained_traveltime : NULL,
            traveltime_used_mask, traveltime_rejected_mask,
            LOCALIZATION_CALCULATE_METHOD_VERSION, config->timing_uncertainty_us,
            &records[record_count]
        );
        localization_calculate_apply_redundancy(
            &records[record_count],
            localization_calculate_degrees_of_freedom(
                constrained_traveltime.picks_used_count,
                dimensionality == LOCALIZATION_DIMENSIONALITY_PLANAR_2D
            )
        );
        snprintf(records[record_count].method_name, sizeof(records[record_count].method_name),
                 "%s", "traveltime_scan_constrained");
        if (!usable) {
            localization_calculate_set_rejection(
                &records[record_count], not_applied_reason, not_applied_failure_class
            );
        }
        free(constrained_traveltime.surface);
        localization_calculate_budget_end(&budget, &records[record_count]);
    ++record_count;

        /* Semblance, constrained. */
        localization_semblance_result_t constrained_semblance;
        memset(&constrained_semblance, 0, sizeof(constrained_semblance));
        esp_err_t constrained_semblance_err = ESP_ERR_INVALID_STATE;
        scan_constraint_applied[1] =
            localization_calculate_budget_begin(&budget, "semblance_scan_constrained", true) &&
            usable && semblance_attempted;
        if (scan_constraint_applied[1]) {
            localization_semblance_channel_t semblance_channels[OBJECT_STORE_POD_COUNT];
            size_t semblance_channel_count = 0U;
            for (size_t ch = 0U; ch < OBJECT_STORE_POD_COUNT; ++ch) {
                if (geometry.pods[ch].computed) {
                    semblance_channels[semblance_channel_count].samples = channel_samples[ch];
                    semblance_channels[semblance_channel_count].sample_count = frame_count;
                    semblance_channels[semblance_channel_count].sample_rate_hz = session_summary.sample_rate_hz;
                    semblance_channels[semblance_channel_count].sensor_position = event_channels[ch].sensor_position;
                    ++semblance_channel_count;
                }
            }
            localization_semblance_config_t constrained_config = {
                .propagation_speed_mps = config->propagation_speed_mps,
                .reference_sample_index = reference_sample_index,
                .window_half_span_samples = config->semblance_window_half_span_samples,
                .refine_divisor = LOCALIZATION_CALCULATE_REFINE_DIVISOR,
            };
            constrained_config.candidate_filter.allowed = localization_calculate_scan_candidate_allowed;
            constrained_config.candidate_filter.context = &occupancy;
            constrained_semblance_err = localization_semblance_search(
                semblance_channels, semblance_channel_count, &semblance_bounds,
                &constrained_config, &constrained_semblance
            );
            summary_out->methods_attempted++;
        }
        localization_calculate_semblance_to_run_record(
            &context,
            (usable && semblance_attempted && constrained_semblance_err == ESP_OK) ? &constrained_semblance : NULL,
            !usable
                ? not_applied_reason
                : (semblance_attempted
                    ? (constrained_semblance_err == ESP_OK
                        ? "scan occupancy excluded every candidate or the search failed"
                        : "scan-constrained semblance search failed")
                    : "no accepted arrival pick to anchor the search window"),
            !usable
                ? not_applied_failure_class
                : (semblance_attempted
                    ? (constrained_semblance_err == ESP_OK
                        ? LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS
                        : LOCALIZATION_FAILURE_CLASS_INTERNAL)
                    : LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN),
            all_channel_mask, (~all_channel_mask) & LOCALIZATION_CALCULATE_ALL_CHANNELS_MASK,
            LOCALIZATION_CALCULATE_METHOD_VERSION, &records[record_count]
        );
        snprintf(records[record_count].method_name, sizeof(records[record_count].method_name),
                 "%s", "semblance_scan_constrained");
        if (usable && constrained_semblance_err == ESP_OK && !constrained_semblance.found) {
            records[record_count].failure_class = LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS;
        }
        localization_calculate_budget_end(&budget, &records[record_count]);
    ++record_count;

        /* Straight-ray methods: candidates never in FREE air, and sensors /
         * combinations whose ray crosses scan-verified empty space dropped. */
        const localization_calculate_scan_ray_context_t ray_context = {
            .occupancy = &occupancy,
            .sensor_clearance_m =
                LOCALIZATION_CALCULATE_SCAN_RAY_CLEARANCE_VOXELS * occupancy.voxel_size_m,
        };

        /* TDCAS, constrained. */
        localization_tdcas_result_t *constrained_tdcas = &workspace->scan_tdcas;
        esp_err_t constrained_tdcas_err = ESP_ERR_INVALID_STATE;
        scan_constraint_applied[2] =
            localization_calculate_budget_begin(&budget, "tdcas_scan_constrained", true) &&
            usable && p_observation_count > 0U;
        if (scan_constraint_applied[2]) {
            const localization_tdcas_config_t constrained_config = {
                .candidate_filter = {
                    .allowed = localization_calculate_scan_candidate_allowed,
                    .context = &occupancy,
                },
                .ray_filter = {
                    .clear = localization_calculate_scan_ray_clear,
                    .context = (void *)&ray_context,
                },
                .velocity_bounds = &config->velocity_range.bounds,
                .timing_uncertainty_us = config->timing_uncertainty_us,
            };
            constrained_tdcas_err = localization_tdcas_solve(
                p_observations, p_observation_count, &constrained_config, constrained_tdcas
            );
            summary_out->methods_attempted++;
        }
        const uint32_t tdcas_obstructed_channels = observation_mask_to_channels(
            constrained_tdcas->sensor_mask_obstructed, workspace->p_channel, p_observation_count
        );
        localization_calculate_tdcas_to_run_record(
            &context, constrained_tdcas_err == ESP_OK ? constrained_tdcas : NULL,
            traveltime_used_mask & ~tdcas_obstructed_channels,
            traveltime_rejected_mask | tdcas_obstructed_channels,
            LOCALIZATION_CALCULATE_METHOD_VERSION, config->timing_uncertainty_us,
            &records[record_count], &summary_out->tdcas_scan_velocity_status
        );
        localization_calculate_apply_redundancy(
            &records[record_count],
            localization_calculate_velocity_free_degrees_of_freedom(
                constrained_tdcas->sensors_used, LOCALIZATION_CALCULATE_TDCAS_UNKNOWNS
            )
        );
        snprintf(records[record_count].method_name, sizeof(records[record_count].method_name),
                 "%s", "tdcas_scan_constrained");
        if (!usable) {
            localization_calculate_set_rejection(
                &records[record_count], not_applied_reason, not_applied_failure_class
            );
        }
        localization_calculate_budget_end(&budget, &records[record_count]);
    ++record_count;

        /* PSAFUVS, constrained. */
        localization_psafuvs_result_t *constrained_psafuvs = &workspace->scan_psafuvs;
        esp_err_t constrained_psafuvs_err = ESP_ERR_INVALID_STATE;
        scan_constraint_applied[3] =
            localization_calculate_budget_begin(&budget, "psafuvs_scan_constrained", true) &&
            usable && ps_observation_count > 0U;
        if (scan_constraint_applied[3]) {
            localization_psafuvs_config_t constrained_config = localization_psafuvs_default_config();
            constrained_config.candidate_filter.allowed = localization_calculate_scan_candidate_allowed;
            constrained_config.candidate_filter.context = &occupancy;
            constrained_config.ray_filter.clear = localization_calculate_scan_ray_clear;
            constrained_config.ray_filter.context = (void *)&ray_context;
            constrained_config.velocity_bounds = &config->velocity_range.bounds;
            constrained_config.timing_uncertainty_us = config->timing_uncertainty_us;
            constrained_psafuvs_err = localization_psafuvs_solve(
                ps_observations, ps_observation_count, &constrained_config, constrained_psafuvs
            );
            summary_out->methods_attempted++;
        }
        const uint32_t psafuvs_obstructed_channels = observation_mask_to_channels(
            constrained_psafuvs->sensor_mask_obstructed, workspace->ps_channel, ps_observation_count
        );
        localization_calculate_psafuvs_to_run_record(
            &context, constrained_psafuvs_err == ESP_OK ? constrained_psafuvs : NULL,
            usable ? psafuvs_not_attempted : not_applied_reason,
            usable ? LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN : not_applied_failure_class,
            ps_used_mask & ~psafuvs_obstructed_channels,
            ((~ps_used_mask) & LOCALIZATION_CALCULATE_ALL_CHANNELS_MASK) | psafuvs_obstructed_channels,
            LOCALIZATION_CALCULATE_METHOD_VERSION, config->timing_uncertainty_us,
            &records[record_count], &summary_out->psafuvs_scan_velocity_status
        );
        localization_calculate_apply_redundancy(
            &records[record_count],
            localization_calculate_velocity_free_degrees_of_freedom(
                2U * constrained_psafuvs->sensors_used, LOCALIZATION_CALCULATE_PSAFUVS_UNKNOWNS
            )
        );
        snprintf(records[record_count].method_name, sizeof(records[record_count].method_name),
                 "%s", "psafuvs_scan_constrained");
        localization_calculate_budget_end(&budget, &records[record_count]);
    ++record_count;

        /* SUEA: rays travel around the scan's empty areas (FREE air is
         * EMPTY, observed surface SOLID, the unseen interior UNKNOWN). */
        localization_suea_result_t *scan_suea = &workspace->scan_suea;
        esp_err_t scan_suea_err = ESP_ERR_INVALID_STATE;
        scan_constraint_applied[4] =
            localization_calculate_budget_begin(&budget, "suea_scan_voids", true) &&
            usable && suea_grid_err == ESP_OK;
        if (scan_constraint_applied[4]) {
            localization_suea_grid_classify(suea_grid, localization_calculate_scan_suea_cell, &occupancy);
            localization_suea_config_t scan_suea_config = suea_config;
            scan_suea_config.candidate_filter.allowed = localization_calculate_scan_candidate_allowed;
            scan_suea_config.candidate_filter.context = &occupancy;
            scan_suea_err = localization_suea_locate(
                suea_grid, suea_picks, OBJECT_STORE_POD_COUNT, &scan_suea_config, scan_suea
            );
            summary_out->methods_attempted++;
        }
        localization_calculate_suea_to_run_record(
            &context, scan_suea_err == ESP_OK ? scan_suea : NULL,
            scan_suea->pick_mask_used != 0U ? scan_suea->pick_mask_used : traveltime_used_mask,
            scan_suea->pick_mask_used != 0U
                ? (~scan_suea->pick_mask_used) & LOCALIZATION_CALCULATE_ALL_CHANNELS_MASK
                : traveltime_rejected_mask,
            LOCALIZATION_CALCULATE_METHOD_VERSION, config->timing_uncertainty_us,
            &records[record_count], &summary_out->suea_scan_velocity_status
        );
        localization_calculate_apply_redundancy(
            &records[record_count],
            localization_calculate_degrees_of_freedom(
                scan_suea->picks_used_count,
                dimensionality == LOCALIZATION_DIMENSIONALITY_PLANAR_2D
            )
        );
        snprintf(records[record_count].method_name, sizeof(records[record_count].method_name),
                 "%s", "suea_scan_voids");
        if (!usable) {
            localization_calculate_set_rejection(
                &records[record_count], not_applied_reason, not_applied_failure_class
            );
        }
        localization_calculate_budget_end(&budget, &records[record_count]);
    ++record_count;

        /* Eikonal travel times around the scan's voids (FREE air is
         * impassable), used three ways: location, semblance focusing with
         * those times, and the Bayesian posterior. */
        const localization_calculate_scan_velocity_context_t scan_velocity = {
            .occupancy = &occupancy,
            .velocity_mps = config->propagation_speed_mps,
            .sample_radius_m = eikonal_model_err == ESP_OK ? 0.5 * eikonal_model->cell_m : 0.0,
            .planar = planar,
        };
        bool scan_fields_ready = false;
        method_allowed = localization_calculate_budget_begin(&budget, "eikonal_scan_voids", true);
        if (usable && eikonal_model_err == ESP_OK && p_observation_count > 0U && method_allowed) {
            localization_eikonal_model_assign(
                eikonal_model, localization_calculate_scan_eikonal_velocity, (void *)&scan_velocity
            );
            scan_fields_ready = localization_eikonal_fields_compute(
                eikonal_model, workspace->eikonal_sensors, participants, &workspace->eikonal_fields
            ) == ESP_OK;
        }
        scan_constraint_applied[5] = scan_fields_ready;
        workspace->eikonal_times.model = eikonal_model;
        workspace->eikonal_times.fields = &workspace->eikonal_fields;
        const char *scan_fields_reason = !usable
            ? not_applied_reason
            : (p_observation_count == 0U
                ? "no accepted arrival pick"
                : "eikonal fields around the scan voids could not be computed");

        localization_eikonal_result_t *scan_eikonal = &workspace->scan_eikonal;
        esp_err_t scan_eikonal_err = ESP_ERR_INVALID_STATE;
        if (scan_fields_ready) {
            localization_eikonal_locate_config_t scan_config = eikonal_config;
            scan_config.candidate_filter.allowed = localization_calculate_scan_candidate_allowed;
            scan_config.candidate_filter.context = &occupancy;
            scan_eikonal_err = localization_eikonal_locate(
                eikonal_model, &workspace->eikonal_fields, workspace->eikonal_picks, participants,
                &scan_config, scan_eikonal
            );
            summary_out->methods_attempted++;
        }
        const uint32_t eikonal_unreachable = observation_mask_to_channels(
            scan_eikonal_err == ESP_OK ? (scan_eikonal->pick_mask_unreachable | scan_eikonal->pick_mask_rejected) : 0U,
            workspace->participant_channel, participants
        );
        localization_calculate_eikonal_to_run_record(
            &context, scan_eikonal_err == ESP_OK ? scan_eikonal : NULL, "eikonal_scan_voids",
            scan_fields_reason,
            !usable ? not_applied_failure_class
                : p_observation_count == 0U ? LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN
                : LOCALIZATION_FAILURE_CLASS_INTERNAL,
            velocity_uncertain,
            traveltime_used_mask & ~eikonal_unreachable, traveltime_rejected_mask | eikonal_unreachable,
            LOCALIZATION_CALCULATE_METHOD_VERSION, config->timing_uncertainty_us,
            &records[record_count]
        );
        localization_calculate_apply_redundancy(
            &records[record_count], localization_calculate_degrees_of_freedom(scan_eikonal->picks_used, planar)
        );
        localization_calculate_budget_end(&budget, &records[record_count]);
    ++record_count;

        /* Semblance focused with the eikonal times (channel index =
         * participant index = field index). */
        localization_semblance_result_t eikonal_semblance;
        memset(&eikonal_semblance, 0, sizeof(eikonal_semblance));
        esp_err_t eikonal_semblance_err = ESP_ERR_INVALID_STATE;
        scan_constraint_applied[6] =
            localization_calculate_budget_begin(&budget, "semblance_eikonal_voids", true) &&
            scan_fields_ready && semblance_attempted;
        if (scan_constraint_applied[6]) {
            localization_semblance_channel_t semblance_channels[OBJECT_STORE_POD_COUNT];
            for (size_t i = 0U; i < participants; ++i) {
                const size_t ch = workspace->participant_channel[i];
                semblance_channels[i].samples = channel_samples[ch];
                semblance_channels[i].sample_count = frame_count;
                semblance_channels[i].sample_rate_hz = session_summary.sample_rate_hz;
                semblance_channels[i].sensor_position = event_channels[ch].sensor_position;
            }
            localization_semblance_config_t eikonal_semblance_config = {
                .propagation_speed_mps = config->propagation_speed_mps,
                .reference_sample_index = reference_sample_index,
                .window_half_span_samples = config->semblance_window_half_span_samples,
                .refine_divisor = LOCALIZATION_CALCULATE_REFINE_DIVISOR,
            };
            eikonal_semblance_config.candidate_filter.allowed = localization_calculate_scan_candidate_allowed;
            eikonal_semblance_config.candidate_filter.context = &occupancy;
            eikonal_semblance_config.travel_time_model.travel_time = localization_eikonal_travel_time;
            eikonal_semblance_config.travel_time_model.context = &workspace->eikonal_times;
            eikonal_semblance_err = localization_semblance_search(
                semblance_channels, participants, &semblance_bounds,
                &eikonal_semblance_config, &eikonal_semblance
            );
            summary_out->methods_attempted++;
        }
        localization_calculate_semblance_to_run_record(
            &context,
            (scan_fields_ready && semblance_attempted && eikonal_semblance_err == ESP_OK && eikonal_semblance.found)
                ? &eikonal_semblance : NULL,
            !scan_fields_ready
                ? scan_fields_reason
                : (semblance_attempted
                    ? (eikonal_semblance_err == ESP_OK
                        ? "scan occupancy excluded every candidate around the voids"
                        : "eikonal-focused semblance call failed")
                    : "no accepted arrival pick to anchor the search window"),
            !scan_fields_ready
                ? (!usable ? not_applied_failure_class
                    : p_observation_count == 0U ? LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN
                    : LOCALIZATION_FAILURE_CLASS_INTERNAL)
                : (semblance_attempted
                    ? (eikonal_semblance_err == ESP_OK
                        ? LOCALIZATION_FAILURE_CLASS_MOVE_SENSORS
                        : LOCALIZATION_FAILURE_CLASS_INTERNAL)
                    : LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN),
            all_channel_mask, (~all_channel_mask) & LOCALIZATION_CALCULATE_ALL_CHANNELS_MASK,
            LOCALIZATION_CALCULATE_METHOD_VERSION, &records[record_count]
        );
        snprintf(records[record_count].method_name, sizeof(records[record_count].method_name),
                 "%s", "semblance_eikonal_voids");
        localization_calculate_budget_end(&budget, &records[record_count]);
    ++record_count;

        /* Bayesian posterior with the eikonal times and the scan's free
         * air as zero prior. */
        localization_bayes_result_t *scan_bayes = &workspace->scan_bayes;
        esp_err_t scan_bayes_err = ESP_ERR_INVALID_STATE;
        scan_constraint_applied[7] =
            localization_calculate_budget_begin(&budget, "bayesian_scan_voids", true) && scan_fields_ready;
        if (scan_constraint_applied[7]) {
            localization_bayes_config_t scan_config = bayes_config;
            scan_config.model_error_relative = LOCALIZATION_CALCULATE_EIKONAL_RELATIVE_ERROR;
            scan_config.model_error_floor_us = localization_calculate_discretization_error_us(
                fmax(eikonal_model->cell_m, occupancy.voxel_size_m), config->propagation_speed_mps
            );
            scan_config.candidate_filter.allowed = localization_calculate_scan_candidate_allowed;
            scan_config.candidate_filter.context = &occupancy;
            scan_config.travel_time_model.travel_time = localization_eikonal_travel_time;
            scan_config.travel_time_model.context = &workspace->eikonal_times;
            scan_bayes_err = localization_bayes_locate(
                workspace->bayes_picks, participants, &extended_bounds, &scan_config, scan_bayes
            );
            summary_out->methods_attempted++;
        }
        localization_calculate_bayes_to_run_record(
            &context, scan_bayes_err == ESP_OK ? scan_bayes : NULL, "bayesian_scan_voids",
            scan_fields_reason,
            !usable ? not_applied_failure_class
                : p_observation_count == 0U ? LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN
                : LOCALIZATION_FAILURE_CLASS_INTERNAL,
            velocity_uncertain,
            traveltime_used_mask, traveltime_rejected_mask,
            LOCALIZATION_CALCULATE_METHOD_VERSION, config->timing_uncertainty_us,
            &records[record_count]
        );
        localization_calculate_apply_redundancy(
            &records[record_count], localization_calculate_degrees_of_freedom(scan_bayes->picks_used, planar)
        );
        localization_calculate_budget_end(&budget, &records[record_count]);
    ++record_count;
        localization_eikonal_fields_free(&workspace->eikonal_fields);

        for (size_t i = 0U; i < LOCALIZATION_CALCULATE_SCAN_METHODS; ++i) {
            localization_calculate_set_scan_provenance(
                &records[scan_record_start + i], scan_constraint_applied[i],
                usable ? &occupancy : NULL
            );
        }

        if (occupancy_err == ESP_OK) {
            scan_occupancy_free(&occupancy);
        }
    }
    if (suea_grid_err == ESP_OK) {
        localization_suea_grid_free(suea_grid);
    }
    if (eikonal_model_err == ESP_OK) {
        localization_eikonal_model_free(eikonal_model);
    }

    free_channel_buffers(channel_samples);

    for (size_t i = 0U; i < record_count; ++i) {
        if (records[i].solved) {
            summary_out->methods_solved++;
        }
        if (analysis_window_truncated) {
            records[i].analysis_window_truncated = true;
            records[i].analysis_window_start_index = 0U;
            records[i].analysis_window_frames = frame_count;
            if (analysis_window_memory_limited) {
                snprintf(
                    records[i].analysis_window_reason,
                    sizeof(records[i].analysis_window_reason), "memory"
                );
            }
        }
    }

    /* --- Persist every individual method's own record, solved or not ----- */

    esp_err_t first_write_err = ESP_OK;
    for (size_t i = 0U; i < record_count; ++i) {
        if (!record_has_rejection_class(&records[i])) {
            first_write_err = ESP_ERR_INVALID_STATE;
            continue;
        }
        const esp_err_t write_err = localization_run_store_write(object_id, session_id, &records[i]);
        if (write_err == ESP_OK) {
            summary_out->methods_persisted++;
        } else if (first_write_err == ESP_OK) {
            first_write_err = write_err;
        }
    }

    /* --- Joint quality-weighted fusion (MAX-138 part 4) ------------------- */

    (void)localization_calculate_budget_begin(&budget, "joint_fusion", false);

    localization_comparison_method_result_t *comparison_methods = workspace->comparison_methods;
    for (size_t i = 0U; i < fused_record_count; ++i) {
        localization_calculate_run_record_to_comparison_method(&records[i], &comparison_methods[i]);
    }

    localization_comparison_result_t *comparison_result = &workspace->comparison_result;
    const esp_err_t comparison_err = localization_comparison_fuse(
        comparison_methods, fused_record_count, comparison_result
    );

    localization_run_record_t *joint_record = &workspace->joint_record;
    localization_calculate_joint_to_run_record(
        &context, comparison_err == ESP_OK ? comparison_result : NULL,
        all_channel_mask, (~all_channel_mask) & LOCALIZATION_CALCULATE_ALL_CHANNELS_MASK,
        LOCALIZATION_CALCULATE_METHOD_VERSION, joint_record
    );
    if (analysis_window_truncated) {
        joint_record->analysis_window_truncated = true;
        joint_record->analysis_window_start_index = 0U;
        joint_record->analysis_window_frames = frame_count;
        if (analysis_window_memory_limited) {
            snprintf(
                joint_record->analysis_window_reason,
                sizeof(joint_record->analysis_window_reason), "memory"
            );
        }
    }
    if (joint_record->solved) {
        summary_out->methods_solved++;
    }

    localization_calculate_budget_end(&budget, joint_record);
    localization_calculate_recommendation(
        records, record_count, &workspace->pick_record, 1U, &summary_out->recommendation
    );

    if (!record_has_rejection_class(joint_record)) {
        first_write_err = ESP_ERR_INVALID_STATE;
        heap_caps_free(workspace);
        return first_write_err;
    }

    const esp_err_t joint_write_err = localization_run_store_write(object_id, session_id, joint_record);
    if (joint_write_err == ESP_OK) {
        summary_out->methods_persisted++;
    } else if (first_write_err == ESP_OK) {
        first_write_err = joint_write_err;
    }

    if (first_write_err == ESP_OK && pick_write_err != ESP_OK) {
        first_write_err = pick_write_err;
    }

    heap_caps_free(workspace);
    return first_write_err;
}
