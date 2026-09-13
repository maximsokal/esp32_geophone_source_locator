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

    char backup[
        OBJECT_RECOVERY_PATH_SIZE
    ];

    char temporary[
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

    err =
        object_recovery_fs_build_path(
            backup,
            sizeof(backup),
            object_directory,
            "object.json.bak"
        );

    if (err != ESP_OK) {
        return err;
    }

    err =
        object_recovery_fs_build_path(
            temporary,
            sizeof(temporary),
            object_directory,
            "object.json.tmp"
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

    if (
        object_recovery_fs_metadata_valid(
            backup,
            object_id
        )
    ) {
        err =
            object_recovery_fs_restore_file(
                backup,
                primary
            );

        if (err != ESP_OK) {
            return err;
        }

        object_recovery_fs_unlink_if_exists(
            temporary
        );

        *valid =
            true;

        *recovered =
            true;

        return ESP_OK;
    }

    if (
        object_recovery_fs_metadata_valid(
            temporary,
            object_id
        )
    ) {
        err =
            object_recovery_fs_restore_file(
                temporary,
                primary
            );

        if (err != ESP_OK) {
            return err;
        }

        object_recovery_fs_unlink_if_exists(
            backup
        );

        *valid =
            true;

        *recovered =
            true;
    }

    return ESP_OK;
}
