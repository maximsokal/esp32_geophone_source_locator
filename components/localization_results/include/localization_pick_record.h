#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "geophone_events.h"

/*
 * MAX-148 (slice 3): a versioned, persisted record of the per-channel
 * arrival picks one localization_calculate_run() invocation actually used
 * -- pure serialize/parse only, exactly like localization_run_record.h's
 * own split (this component owns no file I/O or path convention).
 *
 * Every "Calculate source" click derives fresh picks from that click's own
 * pick_config and the session's raw samples -- unlike the session's frozen
 * geometry/CS pairs (MAX-166/148 slice 2, captured once at recording
 * start), pick evidence is a property of one *computation*, not of the
 * session itself. So this is versioned by computed_at_ms exactly like
 * localization_run_record_t, and a WebUI showing "why did method X reject
 * channel 3" should match it to the run(s) sharing that same
 * computed_at_ms rather than expecting one pick record per session.
 *
 * channels[] reuses geophone_channel_pick_t verbatim (the same struct
 * localization_event_channel_input_t already embeds in
 * localization_event_bridge.h) rather than re-declaring its fields under a
 * new name -- this project already treats that struct as the stable,
 * shared shape for "one channel's pick result".
 */

#define LOCALIZATION_PICK_RECORD_SCHEMA_VERSION 1U

#define LOCALIZATION_PICK_RECORD_OBJECT_ID_SIZE  16U
#define LOCALIZATION_PICK_RECORD_SESSION_ID_SIZE 16U

/* This project's AD7606 channel count (pod index == channel index,
 * confirmed elsewhere in this codebase) -- a local constant rather than a
 * new object_store.h/seismic_buffer.h dependency, matching how every other
 * component in this family (nrf_cs_scheduler.h, seismic_buffer.h,
 * object_store.h) keeps its own copy of this same "4" rather than sharing
 * one canonical definition. */
#define LOCALIZATION_PICK_RECORD_CHANNEL_COUNT 4U


typedef struct
{
    /* False for a channel whose pod had no valid frozen session geometry
     * at all (never a pick candidate); true otherwise, regardless of
     * whether `pick.status == GEOPHONE_PICK_ACCEPTED`. */
    bool participating;
    geophone_channel_pick_t pick;

} localization_pick_channel_t;


typedef struct
{
    uint32_t schema_version;

    char object_id[LOCALIZATION_PICK_RECORD_OBJECT_ID_SIZE];
    char session_id[LOCALIZATION_PICK_RECORD_SESSION_ID_SIZE];
    uint64_t computed_at_ms;

    localization_pick_channel_t channels[LOCALIZATION_PICK_RECORD_CHANNEL_COUNT];

} localization_pick_record_t;


/*
 * Serializes to a newly malloc'd, NUL-terminated JSON string in *json_out
 * (caller frees with free()). Returns ESP_ERR_INVALID_ARG for NULL
 * pointers or a non-finite arrival_time_s/snr on a channel reported
 * GEOPHONE_PICK_ACCEPTED (a non-finite value has no meaning for an
 * accepted pick; a rejected pick's stale/zero fields are never validated,
 * matching geophone_events.h's own "rejection still fills diagnostic
 * fields" contract).
 */
esp_err_t localization_pick_record_to_json(
    const localization_pick_record_t *record,
    char **json_out
);


/*
 * Parses a JSON string produced by localization_pick_record_to_json() (or
 * anything satisfying the same schema). Rejects an unknown/future
 * schema_version rather than guessing at partial data. Returns
 * ESP_ERR_INVALID_RESPONSE for malformed/invalid JSON, ESP_ERR_NOT_FOUND
 * for a recognized-but-unsupported schema_version.
 */
esp_err_t localization_pick_record_from_json(
    const char *json,
    localization_pick_record_t *record
);
