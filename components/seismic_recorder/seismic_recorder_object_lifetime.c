#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "object_store.h"
#include "seismic_recorder.h"
#include "seismic_recorder_internal.h"


static const char *TAG = "SEISMIC_LIFETIME";

/* seismic_recorder.c is compiled with a source-local symbol rename so this
 * wrapper can own the full START transaction without rewriting recorder state
 * machinery. */
esp_err_t seismic_recorder_start_unleased(const char *object_id);

static atomic_flag s_lifetime_gate = ATOMIC_FLAG_INIT;
static object_store_lease_t s_lifetime_lease = {0};


static esp_err_t seismic_recorder_lifetime_release(void)
{
    if (s_lifetime_lease.token_id == 0U) {
        atomic_flag_clear(&s_lifetime_gate);
        return ESP_OK;
    }

    const uint64_t token_id = s_lifetime_lease.token_id;
    const esp_err_t err = object_store_lease_release(&s_lifetime_lease);
    if (err != ESP_OK) {
        /* Fail closed: keep both the exact token and gate owned when release
         * cannot be proven. Object DELETE therefore remains rejected. */
        ESP_LOGE(
            TAG,
            "Recorder object lease release failed token=%llu err=%s",
            (unsigned long long)token_id,
            esp_err_to_name(err)
        );
        (void)token_id;
        return err;
    }

    (void)token_id;
    atomic_flag_clear(&s_lifetime_gate);
    return ESP_OK;
}


esp_err_t seismic_recorder_start(const char *object_id)
{
    if (object_id == NULL || object_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    /* One recorder lifetime owns one token. Holding this gate over the complete
     * underlying START call closes the failure-return window around s_starting
     * and prevents a second caller from replacing the pending ownership. */
    if (atomic_flag_test_and_set(&s_lifetime_gate)) {
        return ESP_ERR_INVALID_STATE;
    }

    object_store_lease_t lease = {0};
    esp_err_t err = object_store_lease_acquire(object_id, &lease);
    if (err != ESP_OK) {
        atomic_flag_clear(&s_lifetime_gate);
        return err;
    }
    s_lifetime_lease = lease;

    /* The lease is authoritative before the original START performs its first
     * object/profile read, and remains owned through session/file creation. */
    err = seismic_recorder_start_unleased(object_id);
    if (err == ESP_OK) {
        return ESP_OK;
    }

    /* The underlying START has already completed every file/session rollback
     * and cleared its s_starting state before returning an error. */
    const esp_err_t release_err = seismic_recorder_lifetime_release();
    return release_err != ESP_OK ? release_err : err;
}


esp_err_t seismic_recorder_lifetime_write_metadata(
    const object_store_profile_t *profile,
    const seismic_recorder_snapshot_t *snapshot,
    const char *status,
    const seismic_recorder_session_participation_t *participation
)
{
    const esp_err_t metadata_err = seismic_recorder_json_write_metadata(
        profile,
        snapshot,
        status,
        participation
    );

    /* The underlying writer synchronously dispatches the recovery-request
     * callback before returning on terminal metadata failure. Releasing here
     * is therefore after raw/LOD close and the callback, while recorder.c has
     * not yet published terminal running=false or STOPPED. */
    const bool terminal =
        status != NULL &&
        (strcmp(status, "complete") == 0 || strcmp(status, "error") == 0);
    if (!terminal) {
        return metadata_err;
    }

    const esp_err_t release_err = seismic_recorder_lifetime_release();
    if (release_err != ESP_OK) {
        return metadata_err != ESP_OK ? metadata_err : release_err;
    }
    return metadata_err;
}
