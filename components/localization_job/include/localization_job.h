#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "localization_calculate.h"

/*
 * "Calculate source" as a background job.
 *
 * A full run (twenty methods, eikonal fields, posterior lattices) takes
 * seconds on the ESP32-S3 and several KiB of stack. It must not run inside
 * an HTTP handler: the HTTPS task has an 8 KiB stack and the WebUI waits
 * at most 12 s for a response. Instead the handler submits a job and
 * returns at once; one dedicated worker task runs it with its own
 * LOCALIZATION_JOB_STACK_BYTES stack, a time budget (skipped methods are
 * persisted with that reason), and publishes progress. The WebUI polls
 * the status: state, the method running, elapsed time, every method's
 * measured duration, and the worker stack's minimum free space (its
 * high-water mark) -- so time and stack are measured on the device on
 * every run, not estimated.
 *
 * One job at a time: submitting while one is queued or running is refused
 * (ESP_ERR_INVALID_STATE, HTTP 409), never silently queued behind it.
 */

/* Worker stack (PSRAM): the orchestrator frame plus its deepest solver
 * chain measure ~6.2 KiB on the host; 16 KiB leaves room for the Xtensa
 * windowed-ABI frames and logging. */
#define LOCALIZATION_JOB_STACK_BYTES 16384U
/* Below this much never-used stack a run is flagged (margin_ok false). */
#define LOCALIZATION_JOB_STACK_MARGIN_BYTES 4096U

#define LOCALIZATION_JOB_DEFAULT_BUDGET_MS 60000U
#define LOCALIZATION_JOB_MIN_BUDGET_MS 1000U
#define LOCALIZATION_JOB_MAX_BUDGET_MS 600000U

#define LOCALIZATION_JOB_ID_SIZE 16U


typedef enum
{
    LOCALIZATION_JOB_IDLE = 0,      /* nothing submitted since boot */
    LOCALIZATION_JOB_QUEUED,
    LOCALIZATION_JOB_RUNNING,
    LOCALIZATION_JOB_SUCCEEDED,
    LOCALIZATION_JOB_FAILED

} localization_job_state_id_t;


typedef struct
{
    localization_job_state_id_t state;
    uint32_t job_id;                /* increments per accepted submit */
    char object_id[LOCALIZATION_JOB_ID_SIZE];
    char session_id[LOCALIZATION_JOB_ID_SIZE];
    uint64_t computed_at_ms;        /* the version of the run's records */

    char current_method[LOCALIZATION_CALCULATE_TIMING_NAME_SIZE];
    uint32_t elapsed_ms;            /* since start (running) or total (done) */
    uint32_t budget_ms;

    esp_err_t error;                /* FAILED: why */
    localization_calculate_summary_t summary;

    uint32_t stack_size_bytes;
    uint32_t stack_free_min_bytes;  /* 0 until a job has finished */
    bool stack_margin_ok;

} localization_job_status_t;


/* Creates the worker task. Idempotent. */
esp_err_t localization_job_init(void);

/*
 * Queues one run. budget_ms 0 selects LOCALIZATION_JOB_DEFAULT_BUDGET_MS;
 * otherwise it must lie in [MIN, MAX]. ESP_ERR_INVALID_STATE while another
 * job is queued or running (or before init), ESP_ERR_INVALID_ARG for bad
 * ids / budget. The config is copied.
 */
esp_err_t localization_job_submit(
    const char *object_id,
    const char *session_id,
    const localization_calculate_config_t *config,
    uint32_t budget_ms,
    uint32_t *job_id_out
);

void localization_job_get_status(localization_job_status_t *status_out);

/* Status as JSON (caller frees with free()). */
esp_err_t localization_job_status_to_json(const localization_job_status_t *status, char **json_out);

/* The current status as JSON: get_status + status_to_json. */
esp_err_t localization_job_status_json(char **json_out);


/* ---- Pure state machine (no RTOS; the worker wraps it in a mutex) ---- */

typedef struct
{
    localization_job_status_t status;
    localization_calculate_config_t config;
    int64_t started_us;

} localization_job_state_t;

void localization_job_state_init(localization_job_state_t *state, uint32_t stack_size_bytes);

esp_err_t localization_job_state_submit(
    localization_job_state_t *state,
    const char *object_id,
    const char *session_id,
    const localization_calculate_config_t *config,
    uint32_t budget_ms,
    uint64_t computed_at_ms
);

/* QUEUED -> RUNNING: copies the job out with its deadline set. */
bool localization_job_state_start(
    localization_job_state_t *state,
    int64_t now_us,
    localization_calculate_config_t *config_out
);

void localization_job_state_progress(localization_job_state_t *state, const char *method_name, int64_t now_us);

void localization_job_state_finish(
    localization_job_state_t *state,
    esp_err_t result,
    const localization_calculate_summary_t *summary,
    uint32_t stack_free_min_bytes,
    int64_t now_us
);

/* A copy with elapsed_ms brought up to now while running. */
void localization_job_state_snapshot(
    const localization_job_state_t *state,
    int64_t now_us,
    localization_job_status_t *status_out
);

const char *localization_job_state_name(localization_job_state_id_t state);
