#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "storage_manager.h"
#include "storage_manager_internal.h"


#define STORAGE_WRITE_TEST_PATH "storage_manager_text_write_contract_tmp.txt"


static unsigned fake_stat_calls;
static unsigned fake_rename_calls;
static unsigned fake_remove_calls;
static unsigned fake_fail_rename_calls[2];
static int fake_fail_remove_call;


esp_err_t storage_manager_operation_begin(void)
{
    return ESP_OK;
}


void storage_manager_operation_end(void)
{
}


static int storage_manager_test_stat(
    const char *path,
    struct stat *info
)
{
    ++fake_stat_calls;
    return stat(path, info);
}


static int storage_manager_test_rename(
    const char *source,
    const char *destination
)
{
    ++fake_rename_calls;
    if (
        fake_rename_calls == fake_fail_rename_calls[0] ||
        fake_rename_calls == fake_fail_rename_calls[1]
    ) {
        errno = EIO;
        return -1;
    }

    return rename(source, destination);
}


static int storage_manager_test_remove(
    const char *path
)
{
    ++fake_remove_calls;
    if ((int)fake_remove_calls == fake_fail_remove_call) {
        errno = EIO;
        return -1;
    }

    return remove(path);
}


static int storage_manager_test_fileno(FILE *file)
{
    (void)file;
    return -1;
}


static int storage_manager_test_fsync(int file_descriptor)
{
    (void)file_descriptor;
    return 0;
}


/* Rename helper-local symbols so both production translation units can share this TU. */
#define TAG storage_manager_text_helper_TAG
#define storage_text_log_io storage_manager_text_helper_log_io
#define storage_text_errno_to_error storage_manager_text_helper_errno_to_error
#define stat(...) storage_manager_test_stat(__VA_ARGS__)
#include "../storage_manager_text_helpers.c"
#undef TAG
#undef storage_text_log_io
#undef storage_text_errno_to_error
#undef stat

#define TAG storage_manager_text_write_TAG
#define stat(...) storage_manager_test_stat(__VA_ARGS__)
#define rename(...) storage_manager_test_rename(__VA_ARGS__)
#define remove(...) storage_manager_test_remove(__VA_ARGS__)
#define fileno(...) storage_manager_test_fileno(__VA_ARGS__)
#define fsync(...) storage_manager_test_fsync(__VA_ARGS__)
#include "../storage_manager_text_write.c"
#undef TAG
#undef stat
#undef rename
#undef remove
#undef fileno
#undef fsync


static void reset_fake_state(void)
{
    fake_stat_calls = 0U;
    fake_rename_calls = 0U;
    fake_remove_calls = 0U;
    fake_fail_rename_calls[0] = 0U;
    fake_fail_rename_calls[1] = 0U;
    fake_fail_remove_call = 0;
    storage_manager_test_last_log_level = '\0';
    storage_manager_test_last_log_message[0] = '\0';
}


static void remove_test_files(void)
{
    (void)remove(STORAGE_WRITE_TEST_PATH);
    (void)remove(STORAGE_WRITE_TEST_PATH ".tmp");
    (void)remove(STORAGE_WRITE_TEST_PATH ".bak");
}


static void write_text_file(
    const char *path,
    const char *text
)
{
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(text, 1U, strlen(text), file) == strlen(text));
    assert(fclose(file) == 0);
}


static bool file_equals(
    const char *path,
    const char *expected
)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;

    char contents[32] = {0};
    const size_t count = fread(contents, 1U, sizeof(contents) - 1U, file);
    const bool read_ok = ferror(file) == 0;
    const int close_result = fclose(file);

    return read_ok && close_result == 0 &&
        count == strlen(expected) && strcmp(contents, expected) == 0;
}


static bool file_exists(
    const char *path
)
{
    struct stat info;
    return stat(path, &info) == 0;
}


static void test_second_rename_failure_restores_original(void)
{
    remove_test_files();
    write_text_file(STORAGE_WRITE_TEST_PATH, "original");
    reset_fake_state();
    fake_fail_rename_calls[0] = 2U;

    const esp_err_t err = storage_manager_atomic_write_text(
        STORAGE_WRITE_TEST_PATH,
        "replacement"
    );
    const bool original_restored = file_equals(STORAGE_WRITE_TEST_PATH, "original");
    const bool backup_removed = !file_exists(STORAGE_WRITE_TEST_PATH ".bak");
    const bool temp_removed = !file_exists(STORAGE_WRITE_TEST_PATH ".tmp");
    const unsigned rename_calls = fake_rename_calls;
    remove_test_files();

    assert(err == ESP_FAIL);
    assert(original_restored);
    assert(backup_removed);
    assert(temp_removed);
    assert(rename_calls == 3U);
}


static void test_rollback_failure_keeps_recoverable_backup(void)
{
    remove_test_files();
    write_text_file(STORAGE_WRITE_TEST_PATH, "original");
    reset_fake_state();
    fake_fail_rename_calls[0] = 2U;
    fake_fail_rename_calls[1] = 3U;

    const esp_err_t err = storage_manager_atomic_write_text(
        STORAGE_WRITE_TEST_PATH,
        "replacement"
    );
    const bool primary_missing = !file_exists(STORAGE_WRITE_TEST_PATH);
    const bool backup_holds_original = file_equals(STORAGE_WRITE_TEST_PATH ".bak", "original");
    const bool temp_removed = !file_exists(STORAGE_WRITE_TEST_PATH ".tmp");
    const bool log_names_recoverable_backup =
        strstr(storage_manager_test_last_log_message, STORAGE_WRITE_TEST_PATH ".bak") != NULL;
    const bool log_records_errno = strstr(storage_manager_test_last_log_message, "errno=5") != NULL;
    remove_test_files();

    assert(err == STORAGE_MANAGER_ERR_BACKUP_ROLLBACK_FAILED);
    assert(STORAGE_MANAGER_ERR_BACKUP_ROLLBACK_FAILED == (esp_err_t)0x10E);
    assert(primary_missing);
    assert(backup_holds_original);
    assert(temp_removed);
    assert(fake_rename_calls == 3U);
    assert(log_names_recoverable_backup);
    assert(log_records_errno);
}


static void test_backup_remove_failure_keeps_installed_write_successful(void)
{
    remove_test_files();
    write_text_file(STORAGE_WRITE_TEST_PATH, "original");
    reset_fake_state();
    fake_fail_remove_call = 3;

    const esp_err_t err = storage_manager_atomic_write_text(
        STORAGE_WRITE_TEST_PATH,
        "replacement"
    );
    const bool new_primary_installed = file_equals(STORAGE_WRITE_TEST_PATH, "replacement");
    const bool old_backup_remains = file_equals(STORAGE_WRITE_TEST_PATH ".bak", "original");
    const esp_err_t recovery_err = storage_manager_recover_backup_locked(STORAGE_WRITE_TEST_PATH);
    const bool primary_still_wins = file_equals(STORAGE_WRITE_TEST_PATH, "replacement");
    const bool backup_still_remains = file_equals(STORAGE_WRITE_TEST_PATH ".bak", "original");
    remove_test_files();

    assert(err == ESP_OK);
    assert(fake_remove_calls == 3U);
    assert(new_primary_installed);
    assert(old_backup_remains);
    assert(recovery_err == ESP_OK);
    assert(primary_still_wins);
    assert(backup_still_remains);
}


int main(void)
{
    test_second_rename_failure_restores_original();
    test_rollback_failure_keeps_recoverable_backup();
    test_backup_remove_failure_keeps_installed_write_successful();
    puts("storage manager text write contract tests passed");
    return 0;
}
