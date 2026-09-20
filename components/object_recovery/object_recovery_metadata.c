#include "object_recovery_fs.h"


esp_err_t object_recovery_fs_prepare_metadata(
    const char *object_directory,
    const char *object_id,
    bool *valid,
    bool *recovered
)
{
    if (
        object_directory == NULL ||
        object_id == NULL ||
        valid == NULL ||
        recovered == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    *valid =
        false;

    *recovered =
        false;

    char primary[
        OBJECT_RECOVERY_PATH_SIZE
    ];

    /*
     * Only one sidecar path is ever live at a time. A valid primary never
     * needs a sidecar path at all; otherwise this buffer holds ".bak" first
     * and is rebuilt in place to hold ".tmp" (or vice versa) exactly when
     * that specific sidecar is next needed, instead of keeping three
     * OBJECT_RECOVERY_PATH_SIZE buffers live simultaneously.
     */
    char sidecar[
        OBJECT_RECOVERY_PATH_SIZE
    ];

    esp_err_t err =
        object_recovery_fs_build_path(
            primary,
            sizeof(primary),
            object_directory,
            "object.json"
        );

    if (err != ESP_OK) {
        return err;
    }

    if (
        object_recovery_fs_metadata_valid(
            primary,
            object_id
        )
    ) {
        /*
         * A valid primary is authoritative. Do not spend another FAT pathname
         * operation deleting an uncommitted/stale .tmp while the display is
         * quiesced for this recovery window. storage_manager_atomic_write_text()
         * always removes the same .tmp before creating the next transaction,
         * so leaving it here cannot block or contaminate a future committed
         * write. Recovery only consults sidecars when the primary is invalid.
         */
        *valid =
            true;

        return ESP_OK;
    }

    err =
        object_recovery_fs_build_path(
            sidecar,
            sizeof(sidecar),
            object_directory,
            "object.json.bak"
        );

    if (err != ESP_OK) {
        return err;
    }

    if (
        object_recovery_fs_metadata_valid(
            sidecar,
            object_id
        )
    ) {
        err =
            object_recovery_fs_restore_file(
                sidecar,
                primary
            );

        if (err != ESP_OK) {
            return err;
        }

        err =
            object_recovery_fs_build_path(
                sidecar,
                sizeof(sidecar),
                object_directory,
                "object.json.tmp"
            );

        if (err != ESP_OK) {
            return err;
        }

        object_recovery_fs_unlink_if_exists(
            sidecar
        );

        *valid =
            true;

        *recovered =
            true;

        return ESP_OK;
    }

    err =
        object_recovery_fs_build_path(
            sidecar,
            sizeof(sidecar),
            object_directory,
            "object.json.tmp"
        );

    if (err != ESP_OK) {
        return err;
    }

    if (
        object_recovery_fs_metadata_valid(
            sidecar,
            object_id
        )
    ) {
        err =
            object_recovery_fs_restore_file(
                sidecar,
                primary
            );

        if (err != ESP_OK) {
            return err;
        }

        err =
            object_recovery_fs_build_path(
                sidecar,
                sizeof(sidecar),
                object_directory,
                "object.json.bak"
            );

        if (err != ESP_OK) {
            return err;
        }

        object_recovery_fs_unlink_if_exists(
            sidecar
        );

        *valid =
            true;

        *recovered =
            true;
    }

    return ESP_OK;
}
