#include "seismic_recorder.h"

#include <string.h>

#include "seismic_recorder_internal.h"


esp_err_t seismic_recorder_delete_session(
    const char *object_id,
    const char *session_id
)
{
    if (
        object_id == NULL ||
        object_id[0] == '\0' ||
        session_id == NULL ||
        session_id[0] == '\0'
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    seismic_recorder_snapshot_t snapshot;
    esp_err_t err = seismic_recorder_get_snapshot(&snapshot);
    if (err != ESP_OK) {
        return err;
    }

    if (snapshot.running) {
        return ESP_ERR_INVALID_STATE;
    }

    if (
        snapshot.session_id[0] != '\0' &&
        strcmp(snapshot.object_id, object_id) == 0 &&
        strcmp(snapshot.session_id, session_id) == 0 &&
        snapshot.state == SEISMIC_RECORDER_STOPPING
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    return seismic_recorder_fs_delete_session(
        object_id,
        session_id
    );
}
