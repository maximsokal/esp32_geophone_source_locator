#include "storage_manager.h"

#include <errno.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "storage_manager_internal.h"
#include "storage_manager_slow_op_policy.h"


static const char *TAG =
    "STORAGE_READ";


/* This source runs under the storage operation mutex, so its slot is serialized. */
static storage_manager_slow_op_state_t s_slow_op_state;


enum
{
    STORAGE_READ_SLOW_OP_SOURCE_RECOVER_BACKUP = 0U
};


static int64_t storage_read_log_io(
    const char *operation,
    const char *path,
    int result,
    int64_t started_us
)
{
    const int64_t elapsed_us = esp_timer_get_time() - started_us;
    ESP_LOGI(
        TAG,
        "IO_TIMING: %s path=%s result=%d elapsed=%lld us",
        operation,
        path,
        result,
        (long long)elapsed_us
    );

    return elapsed_us;
}


static void storage_read_log_slow_op(
    const char *operation,
    int64_t elapsed_us
)
{
    const int64_t now_us = esp_timer_get_time();
    const uint32_t now_ms = now_us > 0
        ? (uint32_t)(now_us / 1000LL)
        : 0U;
    const uint32_t elapsed_ms =
        storage_manager_slow_op_elapsed_ms(elapsed_us);

    if (
        storage_manager_slow_op_should_warn(
            &s_slow_op_state,
            STORAGE_READ_SLOW_OP_SOURCE_RECOVER_BACKUP,
            now_ms,
            elapsed_ms
        )
    ) {
        ESP_LOGW(
            TAG,
            "STORAGE_SLOW op=%s elapsed_ms=%u",
            operation,
            (unsigned)elapsed_ms
        );
    }
}


static esp_err_t storage_read_errno_to_error(
    int error_number
)
{
    if (error_number == ENOENT) {
        return ESP_ERR_NOT_FOUND;
    }

    if (error_number == ENOMEM) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_FAIL;
}


static esp_err_t storage_manager_read_text_impl(
    const char *path,
    char *buffer,
    size_t buffer_size,
    bool recover_backup
)
{
    if (
        path == NULL ||
        buffer == NULL ||
        buffer_size < 2
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t started_us =
        esp_timer_get_time();

    esp_err_t err =
        storage_manager_operation_begin();

    storage_read_log_io(
        "lock-wait",
        path,
        (int)err,
        started_us
    );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Storage is not ready"
        );

        return err;
    }

    if (recover_backup) {
        started_us =
            esp_timer_get_time();

        err =
            storage_manager_recover_backup_locked(
                path
            );

        const int64_t recovery_elapsed_us = storage_read_log_io(
            "recover-backup",
            path,
            (int)err,
            started_us
        );
        storage_read_log_slow_op(
            "recover-backup",
            recovery_elapsed_us
        );

        if (err != ESP_OK) {
            storage_manager_operation_end();

            return err;
        }
    }

    started_us =
        esp_timer_get_time();

    FILE *file =
        fopen(
            path,
            "r"
        );

    const int open_errno =
        file == NULL
            ? errno
            : 0;

    storage_read_log_io(
        "fopen",
        path,
        file == NULL ? -1 : 0,
        started_us
    );

    if (file == NULL) {
        storage_manager_operation_end();

        if (open_errno == ENOENT) {
            ESP_LOGD(
                TAG,
                "Could not open %s: errno=%d",
                path,
                open_errno
            );
        } else {
            ESP_LOGE(
                TAG,
                "Could not open %s: errno=%d",
                path,
                open_errno
            );
        }

        return storage_read_errno_to_error(
            open_errno
        );
    }

    started_us =
        esp_timer_get_time();

    size_t count =
        fread(
            buffer,
            1,
            buffer_size - 1,
            file
        );

    const bool read_failed =
        ferror(file) != 0;

    const int read_errno =
        read_failed
            ? errno
            : 0;

    storage_read_log_io(
        "fread",
        path,
        read_failed ? -1 : (int)count,
        started_us
    );

    if (read_failed) {
        started_us =
            esp_timer_get_time();

        const int close_result =
            fclose(
                file
            );

        storage_read_log_io(
            "fclose-after-read-error",
            path,
            close_result,
            started_us
        );

        storage_manager_operation_end();

        ESP_LOGE(
            TAG,
            "Read failed for %s: errno=%d",
            path,
            read_errno
        );

        return storage_read_errno_to_error(
            read_errno
        );
    }

    buffer[count] =
        '\0';

    if (
        count ==
        buffer_size - 1
    ) {
        started_us =
            esp_timer_get_time();

        int next_character =
            fgetc(
                file
            );

        /* EOF is also what fgetc() returns on an I/O error, so tell them apart. */
        const bool probe_failed =
            next_character == EOF &&
            ferror(file) != 0;

        const int probe_errno =
            probe_failed
                ? errno
                : 0;

        storage_read_log_io(
            "fgetc-size-check",
            path,
            probe_failed ? -1 : next_character,
            started_us
        );

        if (probe_failed) {
            started_us =
                esp_timer_get_time();

            const int close_result =
                fclose(
                    file
                );

            storage_read_log_io(
                "fclose-after-read-error",
                path,
                close_result,
                started_us
            );

            storage_manager_operation_end();

            ESP_LOGE(
                TAG,
                "Read failed for %s: errno=%d",
                path,
                probe_errno
            );

            return storage_read_errno_to_error(
                probe_errno
            );
        }

        if (
            next_character !=
            EOF
        ) {
            started_us =
                esp_timer_get_time();

            const int close_result =
                fclose(
                    file
                );

            storage_read_log_io(
                "fclose-after-size-error",
                path,
                close_result,
                started_us
            );

            storage_manager_operation_end();

            ESP_LOGE(
                TAG,
                "Text file is larger than buffer: %s",
                path
            );

            return ESP_ERR_INVALID_SIZE;
        }
    }

    started_us =
        esp_timer_get_time();

    const int close_result =
        fclose(
            file
        );

    storage_read_log_io(
        "fclose",
        path,
        close_result,
        started_us
    );

    storage_manager_operation_end();

    /*
     * Preserve the original read semantics: close timing is
     * diagnostic only for this bring-up investigation.
     */
    return ESP_OK;
}


esp_err_t storage_manager_read_text(
    const char *path,
    char *buffer,
    size_t buffer_size
)
{
    return storage_manager_read_text_impl(
        path,
        buffer,
        buffer_size,
        true
    );
}


esp_err_t storage_manager_read_text_known_present(
    const char *path,
    char *buffer,
    size_t buffer_size
)
{
    return storage_manager_read_text_impl(
        path,
        buffer,
        buffer_size,
        false
    );
}
