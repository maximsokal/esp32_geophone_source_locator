#if !defined(ESP_PLATFORM) && !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "object_recovery_fs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "cJSON.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "object_format.h"


#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif


static const char *TAG =
    "OBJECT_RECOVERY_IO";


static void object_recovery_log_io(
    const char *operation,
    const char *path,
    int result,
    int64_t started_us
)
{
    ESP_LOGI(
        TAG,
        "IO_TIMING: %s path=%s result=%d elapsed=%lld us",
        operation,
        path,
        result,
        (long long)(
            esp_timer_get_time() -
            started_us
        )
    );
}


static int object_recovery_open_file_stat(
    FILE *file,
    const char *path,
    struct stat *info
)
{
    if (file == NULL || path == NULL || info == NULL) {
        return -1;
    }

#ifdef _WIN32
    /* Keep the native MSVC host-contract build portable. Device firmware and
     * Linux CI take the descriptor-based path below; Windows timing is not a
     * physical-SD performance signal. */
    return stat(path, info);
#else
    const int descriptor = fileno(file);
    return descriptor >= 0
        ? fstat(descriptor, info)
        : -1;
#endif
}


bool object_recovery_fs_metadata_valid(
    const char *path,
    const char *expected_id,
    const char **reason_out
)
{
    if (reason_out != NULL) *reason_out = "not_checked";
    if (
        path == NULL ||
        !object_format_id_is_valid(expected_id)
    ) {
        if (reason_out != NULL) *reason_out = "invalid_argument";
        return false;
    }

    /*
     * Open first, then inspect metadata through the already-open descriptor.
     * On SDSPI/FATFS a pathname lookup is expensive; the old stat(path) +
     * fopen(path) pair paid for two directory walks before reading a tiny JSON
     * file. fstat(fileno(file)) preserves type/size validation while avoiding
     * that second path lookup on the ESP32.
     */
    int64_t started_us =
        esp_timer_get_time();

    FILE *file =
        fopen(
            path,
            "rb"
        );

    object_recovery_log_io(
        "metadata-fopen",
        path,
        file == NULL ? -1 : 0,
        started_us
    );

    if (file == NULL) {
        if (reason_out != NULL) {
            *reason_out = errno == ENOENT ? "not_found" : "open_error";
        }
        return false;
    }

    struct stat info;
    started_us =
        esp_timer_get_time();

    const int stat_result =
        object_recovery_open_file_stat(
            file,
            path,
            &info
        );

    object_recovery_log_io(
        "metadata-fstat",
        path,
        stat_result,
        started_us
    );

    if (
        stat_result != 0 ||
        !S_ISREG(info.st_mode) ||
        info.st_size <= 0 ||
        info.st_size > OBJECT_RECOVERY_METADATA_MAX_SIZE
    ) {
        if (reason_out != NULL) {
            *reason_out = stat_result != 0
                ? "stat_error"
                : !S_ISREG(info.st_mode)
                    ? "not_regular"
                    : "invalid_size";
        }
        started_us =
            esp_timer_get_time();

        const int close_result =
            fclose(
                file
            );

        object_recovery_log_io(
            "metadata-fclose-after-stat-error",
            path,
            close_result,
            started_us
        );

        return false;
    }

    size_t size =
        (size_t)
            info.st_size;

    char *buffer =
        malloc(
            size + 1
        );

    if (buffer == NULL) {
        if (reason_out != NULL) *reason_out = "no_memory";
        started_us =
            esp_timer_get_time();

        const int close_result =
            fclose(
                file
            );

        object_recovery_log_io(
            "metadata-fclose-after-alloc-error",
            path,
            close_result,
            started_us
        );

        return false;
    }

    started_us =
        esp_timer_get_time();

    size_t read_count =
        fread(
            buffer,
            1,
            size,
            file
        );

    object_recovery_log_io(
        "metadata-fread",
        path,
        (int)read_count,
        started_us
    );
    const bool read_error = ferror(file) != 0;

    started_us =
        esp_timer_get_time();

    const int close_result =
        fclose(
            file
        );

    object_recovery_log_io(
        "metadata-fclose",
        path,
        close_result,
        started_us
    );

    if (read_count != size || read_error) {
        free(
            buffer
        );

        if (reason_out != NULL) *reason_out = "short_read";
        return false;
    }

    buffer[size] =
        '\0';

    started_us =
        esp_timer_get_time();

    cJSON *root =
        cJSON_Parse(
            buffer
        );

    object_recovery_log_io(
        "metadata-json-parse",
        path,
        root == NULL ? -1 : 0,
        started_us
    );

    free(
        buffer
    );

    if (root == NULL) {
        if (reason_out != NULL) *reason_out = "invalid_json";
        return false;
    }

    const cJSON *schema_version =
        cJSON_GetObjectItemCaseSensitive(
            root,
            "schema_version"
        );

    const cJSON *id =
        cJSON_GetObjectItemCaseSensitive(
            root,
            "id"
        );

    const char *reason = "valid";
    bool valid = cJSON_IsNumber(schema_version);
    if (!valid) {
        reason = "invalid_schema";
    }
    else if (!object_format_schema_is_supported(schema_version->valueint)) {
        valid = false;
        reason = "unsupported_schema";
    }
    else if (
        !cJSON_IsString(id) ||
        id->valuestring == NULL ||
        !object_format_id_is_valid(id->valuestring)
    ) {
        valid = false;
        reason = "invalid_identity";
    }
    else if (!object_format_metadata_identity_is_valid(
                 schema_version->valueint,
                 id->valuestring,
                 expected_id
             )) {
        valid = false;
        reason = "wrong_identity";
    }

    cJSON_Delete(
        root
    );

    /*
     * fclose result is logged above but intentionally does not
     * change the legacy metadata-validity semantics.
     */
    if (reason_out != NULL) *reason_out = reason;
    return valid;
}
