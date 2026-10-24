#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_err.h"


#define OBJECT_RECOVERY_PATH_SIZE          256
#define OBJECT_RECOVERY_METADATA_MAX_SIZE 16384

typedef struct
{
    const char *primary;
    const char *backup;
    const char *temporary;
} object_recovery_metadata_attempts_t;


esp_err_t object_recovery_fs_build_path(
    char *buffer,
    size_t buffer_size,
    const char *directory,
    const char *file_name
);


/*
 * Outcome of judging one metadata candidate. Only CORRUPT is evidence that the
 * file's content is unusable. UNREADABLE means the check could not be
 * completed (out of memory -- including inside the JSON parser --, open/stat/
 * read failure, bad argument): nothing may be restored, replaced or deleted on
 * the strength of it; the caller must report an error and retry later.
 */
typedef enum
{
    OBJECT_RECOVERY_METADATA_VALID = 0,
    OBJECT_RECOVERY_METADATA_MISSING,     /* ENOENT */
    OBJECT_RECOVERY_METADATA_CORRUPT,     /* read cleanly, content unusable */
    OBJECT_RECOVERY_METADATA_UNREADABLE   /* could not be judged */
} object_recovery_metadata_status_t;


object_recovery_metadata_status_t object_recovery_fs_metadata_check(
    const char *path,
    const char *expected_id,
    const char **reason_out
);


/* True only for OBJECT_RECOVERY_METADATA_VALID. */
bool object_recovery_fs_metadata_valid(
    const char *path,
    const char *expected_id,
    const char **reason_out
);


esp_err_t object_recovery_fs_unlink_if_exists(
    const char *path
);


/*
 * Replaces destination with source without ever leaving the destination path
 * empty on failure. FATFS cannot rename over an existing file (FR_EXIST), so a
 * present destination is first renamed aside, then source takes its place; if
 * that second rename fails the destination is renamed back. The caller must
 * only pass a destination that was proven corrupt (or is missing).
 */
esp_err_t object_recovery_fs_restore_file(
    const char *source,
    const char *destination
);


esp_err_t object_recovery_fs_prepare_metadata(
    const char *object_directory,
    const char *object_id,
    bool *valid,
    bool *recovered,
    object_recovery_metadata_attempts_t *attempts
);

/* Host fault-injection seams; production maps straight to libc. */
#ifdef OBJECT_RECOVERY_TEST_FAULTS
void *object_recovery_test_malloc(size_t size);
FILE *object_recovery_test_fopen(const char *path, const char *mode);
int object_recovery_test_rename(const char *source, const char *destination);
#define OBJECT_RECOVERY_MALLOC(size) object_recovery_test_malloc(size)
#define OBJECT_RECOVERY_FOPEN(path, mode) object_recovery_test_fopen((path), (mode))
#define OBJECT_RECOVERY_RENAME(source, destination) object_recovery_test_rename((source), (destination))
#else
#define OBJECT_RECOVERY_MALLOC(size) malloc(size)
#define OBJECT_RECOVERY_FOPEN(path, mode) fopen((path), (mode))
#define OBJECT_RECOVERY_RENAME(source, destination) rename((source), (destination))
#endif
