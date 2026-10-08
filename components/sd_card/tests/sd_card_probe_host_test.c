#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_card_info.h"
#include "sd_card_internal.h"


static bool s_mounted;
static bool s_fail_create;
static bool s_fail_write;
static bool s_fail_flush;
static unsigned s_fail_close_call;
static bool s_fail_read_open;
static bool s_short_read;
static bool s_corrupt_read;
static bool s_fail_read;
static bool s_fail_remove;
static unsigned s_open_calls;
static int s_read_open_errno;
static unsigned s_write_calls;
static unsigned s_flush_calls;
static unsigned s_close_calls;
static unsigned s_read_calls;
static unsigned s_remove_calls;
static char s_last_path[128];
static char s_last_mode[8];
static char s_last_log[512];
static char s_last_log_tag[32];
static char s_all_logs[2048];
static unsigned s_log_calls;
static int s_write_errno;
static unsigned s_diag_stat_calls;
static unsigned s_diag_opendir_calls;
static unsigned s_diag_readdir_calls;
static unsigned s_diag_closedir_calls;
static unsigned s_delay_calls;
static TickType_t s_last_delay_ticks;
static unsigned s_info_calls;
static unsigned s_card_status_calls;
static size_t s_fake_dir_index;
static struct dirent s_fake_dir_entries[2];
static int s_fake_dir_token;
static sdmmc_card_t s_probe_card;
static const char *s_local_path = "sd_card_probe_host.tmp";


static void reset_fakes(void)
{
    s_mounted = true;
    s_fail_create = false;
    s_fail_write = false;
    s_fail_flush = false;
    s_fail_close_call = 0U;
    s_fail_read_open = false;
    s_read_open_errno = EIO;
    s_short_read = false;
    s_corrupt_read = false;
    s_fail_read = false;
    s_fail_remove = false;
    s_open_calls = 0U;
    s_write_calls = 0U;
    s_flush_calls = 0U;
    s_close_calls = 0U;
    s_read_calls = 0U;
    s_remove_calls = 0U;
    s_last_path[0] = '\0';
    s_last_mode[0] = '\0';
    s_last_log[0] = '\0';
    s_last_log_tag[0] = '\0';
    s_all_logs[0] = '\0';
    s_log_calls = 0U;
    s_diag_stat_calls = 0U;
    s_diag_opendir_calls = 0U;
    s_diag_readdir_calls = 0U;
    s_diag_closedir_calls = 0U;
    s_delay_calls = 0U;
    s_last_delay_ticks = 0U;
    s_info_calls = 0U;
    s_card_status_calls = 0U;
    s_fake_dir_index = 0U;
    memset(s_fake_dir_entries, 0, sizeof(s_fake_dir_entries));
    (void)snprintf(s_fake_dir_entries[0].d_name, sizeof(s_fake_dir_entries[0].d_name), "%s", "KEEP.TXT");
    (void)snprintf(s_fake_dir_entries[1].d_name, sizeof(s_fake_dir_entries[1].d_name), "%s", "MISSING.TMP");
    memset(&s_probe_card, 0, sizeof(s_probe_card));
    s_probe_card.is_mem = 1U;
    s_write_errno = ENOSPC;
    (void)remove(s_local_path);
    errno = EINVAL; /* Deliberately stale: the probe must clear it per call. */
}


static FILE *probe_fake_fopen(const char *path, const char *mode)
{
    ++s_open_calls;
    (void)snprintf(s_last_path, sizeof(s_last_path), "%s", path);
    (void)snprintf(s_last_mode, sizeof(s_last_mode), "%s", mode);
    if (s_fail_create && strcmp(mode, "wb") == 0) {
        errno = EACCES;
        return NULL;
    }
    if (s_fail_read_open && strcmp(mode, "rb") == 0) {
        errno = s_read_open_errno;
        return NULL;
    }
    return fopen(s_local_path, mode);
}


static size_t probe_fake_fwrite(const void *buffer, size_t size, size_t count, FILE *file)
{
    ++s_write_calls;
    if (s_fail_write) {
        errno = s_write_errno;
        return 0U;
    }
    return fwrite(buffer, size, count, file);
}


static int probe_fake_fflush(FILE *file)
{
    ++s_flush_calls;
    const int result = fflush(file);
    if (s_fail_flush) {
        errno = EIO;
    }
    return s_fail_flush ? EOF : result;
}


static int probe_fake_fclose(FILE *file)
{
    ++s_close_calls;
    const int result = fclose(file);
    if (s_fail_close_call == s_close_calls) {
        errno = EIO;
        return EOF;
    }
    return result;
}


static size_t probe_fake_fread(void *buffer, size_t size, size_t count, FILE *file)
{
    ++s_read_calls;
    if (s_fail_read) {
        errno = EIO;
        return 0U;
    }
    const size_t result = fread(buffer, size, count, file);
    if (s_corrupt_read && result > 0U) {
        ((char *)buffer)[0] ^= 0x01;
    }
    return s_short_read && result > 0U ? result - 1U : result;
}


static int probe_fake_ferror(FILE *file)
{
    return s_fail_read ? 1 : ferror(file);
}


static int probe_fake_remove(const char *path)
{
    ++s_remove_calls;
    (void)snprintf(s_last_path, sizeof(s_last_path), "%s", path);
    const int result = remove(s_local_path);
    if (s_fail_remove) {
        errno = EACCES;
    }
    return s_fail_remove ? EOF : result;
}


static int probe_fake_stat(const char *path, struct stat *status)
{
    ++s_diag_stat_calls;
    memset(status, 0, sizeof(*status));
    if (strcmp(path, "/sdcard/KEEP.TXT") == 0) {
        status->st_size = 17;
        return 0;
    }
    errno = ENOENT;
    return -1;
}


static DIR *probe_fake_opendir(const char *path)
{
    ++s_diag_opendir_calls;
    assert(strcmp(path, "/sdcard") == 0);
    s_fake_dir_index = 0U;
    return (DIR *)&s_fake_dir_token;
}


static struct dirent *probe_fake_readdir(DIR *directory)
{
    ++s_diag_readdir_calls;
    assert(directory == (DIR *)&s_fake_dir_token);
    if (s_fake_dir_index >= sizeof(s_fake_dir_entries) / sizeof(s_fake_dir_entries[0])) {
        return NULL;
    }
    return &s_fake_dir_entries[s_fake_dir_index++];
}


static int probe_fake_closedir(DIR *directory)
{
    ++s_diag_closedir_calls;
    assert(directory == (DIR *)&s_fake_dir_token);
    return 0;
}


static void probe_fake_vTaskDelay(TickType_t ticks)
{
    ++s_delay_calls;
    s_last_delay_ticks = ticks;
}


#define fopen probe_fake_fopen
#define fwrite probe_fake_fwrite
#define fflush probe_fake_fflush
#define fclose probe_fake_fclose
#define fread probe_fake_fread
#define ferror probe_fake_ferror
#define remove probe_fake_remove
#define stat(path, status) probe_fake_stat((path), (status))
#define opendir probe_fake_opendir
#define readdir probe_fake_readdir
#define closedir probe_fake_closedir
#define vTaskDelay probe_fake_vTaskDelay
#include "../sd_card_diagnostics.c"
#undef fopen
#undef fwrite
#undef fflush
#undef fclose
#undef fread
#undef ferror
#undef remove
#undef stat
#undef opendir
#undef readdir
#undef closedir
#undef vTaskDelay


const char *esp_err_to_name(esp_err_t error)
{
    (void)error;
    return "host";
}


void fake_esp_log(const char *tag, const char *format, ...)
{
    ++s_log_calls;
    (void)snprintf(s_last_log_tag, sizeof(s_last_log_tag), "%s", tag);
    va_list args;
    va_start(args, format);
    (void)vsnprintf(s_last_log, sizeof(s_last_log), format, args);
    va_end(args);
    const size_t used = strlen(s_all_logs);
    if (used < sizeof(s_all_logs)) {
        (void)snprintf(s_all_logs + used, sizeof(s_all_logs) - used, "%s\n", s_last_log);
    }
}


bool sd_card_is_mounted(void)
{
    return s_mounted;
}


sdmmc_card_t *sd_card_internal_card(void)
{
    return &s_probe_card;
}


esp_err_t sd_card_get_info(sd_card_info_t *info)
{
    ++s_info_calls;
    assert(info != NULL);
    info->capacity_bytes = 32U * 1024U * 1024U;
    info->total_bytes = 30U * 1024U * 1024U;
    info->free_bytes = 12U * 1024U * 1024U;
    return ESP_OK;
}


esp_err_t sdmmc_get_status(sdmmc_card_t *card)
{
    ++s_card_status_calls;
    assert(card == &s_probe_card);
    return ESP_OK;
}


esp_err_t sd_card_diagnostics_write_file(const char *path, const char *text)
{
    (void)path;
    (void)text;
    return ESP_OK;
}


esp_err_t sd_card_diagnostics_read_file(const char *path, char *buffer, size_t buffer_size)
{
    (void)path;
    (void)buffer;
    (void)buffer_size;
    return ESP_OK;
}


static void test_probe_write_read_compare_and_remove(void)
{
    reset_fakes();
    const sd_card_probe_result_t result = sd_card_write_readback_probe();
    assert(result.stage == SD_CARD_PROBE_STAGE_COMPLETE);
    assert(result.err == ESP_OK);
    assert(result.sys_errno == 0);
    assert(s_log_calls == 0U);
    assert(s_open_calls == 2U);
    assert(s_write_calls == 1U);
    assert(s_flush_calls == 1U);
    assert(s_close_calls == 2U);
    assert(s_read_calls == 1U);
    assert(s_remove_calls == 1U);
    assert(s_diag_stat_calls == 0U);
    assert(s_diag_opendir_calls == 0U);
    assert(s_diag_readdir_calls == 0U);
    assert(s_diag_closedir_calls == 0U);
    assert(s_delay_calls == 0U);
    assert(s_info_calls == 0U);
    assert(s_card_status_calls == 0U);
    assert(strcmp(s_last_path, "/sdcard/SDPROBE.TMP") == 0);
    assert(fopen(s_local_path, "rb") == NULL);
}


static void test_unmounted_card_is_typed_failure_without_io(void)
{
    reset_fakes();
    s_mounted = false;
    const sd_card_probe_result_t result = sd_card_write_readback_probe();
    assert(result.stage == SD_CARD_PROBE_STAGE_MOUNT_CHECK);
    assert(result.err == ESP_ERR_INVALID_STATE);
    assert(s_open_calls == 0U);
    assert(s_remove_calls == 0U);
    assert(result.sys_errno == 0);
    assert(strstr(s_last_log, "stage=MOUNT_CHECK") != NULL);
}


static void test_create_and_write_failures_clean_temporary_file(void)
{
    reset_fakes();
    s_fail_create = true;
    sd_card_probe_result_t result = sd_card_write_readback_probe();
    assert(result.stage == SD_CARD_PROBE_STAGE_CREATE);
    assert(result.err == ESP_FAIL);
    assert(result.sys_errno == EACCES);
    assert(s_remove_calls == 1U);

    reset_fakes();
    s_fail_write = true;
    result = sd_card_write_readback_probe();
    assert(result.stage == SD_CARD_PROBE_STAGE_WRITE);
    assert(result.err == ESP_FAIL);
    assert(result.sys_errno == ENOSPC);
    assert(s_remove_calls == 1U);
}


static void test_compare_and_remove_failures_are_reported(void)
{
    reset_fakes();
    s_corrupt_read = true;
    sd_card_probe_result_t result = sd_card_write_readback_probe();
    assert(result.stage == SD_CARD_PROBE_STAGE_COMPARE);
    assert(result.err == ESP_FAIL);
    assert(result.sys_errno == 0);
    assert(s_remove_calls == 1U);

    reset_fakes();
    s_fail_remove = true;
    result = sd_card_write_readback_probe();
    assert(result.stage == SD_CARD_PROBE_STAGE_REMOVE);
    assert(result.err == ESP_FAIL);
    assert(result.sys_errno == EACCES);
}

static void assert_failure(sd_card_probe_stage_t stage, int error)
{
    const sd_card_probe_result_t result = sd_card_write_readback_probe();
    assert(result.stage == stage);
    assert(result.err == ESP_FAIL);
    assert(result.sys_errno == error);
    assert(s_remove_calls == 1U);
    assert(s_log_calls >= 1U);
    assert(s_fail_read_open ? s_log_calls > 1U : s_log_calls == 1U);
    char expected_stage[48];
    char expected_errno[32];
    (void)snprintf(expected_stage, sizeof(expected_stage), "stage=%s", sd_card_probe_stage_name(stage));
    (void)snprintf(expected_errno, sizeof(expected_errno), "errno=%d (", error);
    assert(strstr(s_last_log, expected_stage) != NULL);
    assert(strstr(s_last_log, expected_errno) != NULL);
    assert(strstr(s_last_log, "path=/sdcard/SDPROBE.TMP") != NULL);
}


static void test_read_open_enoent_runs_bounded_diagnostics_without_changing_result(void)
{
    reset_fakes();
    s_fail_read_open = true;
    s_read_open_errno = ENOENT;

    const sd_card_probe_result_t result = sd_card_write_readback_probe();
    assert(result.stage == SD_CARD_PROBE_STAGE_READ);
    assert(result.err == ESP_FAIL);
    assert(result.sys_errno == ENOENT);
    assert(s_open_calls == 3U);
    assert(s_diag_stat_calls == 3U);
    assert(s_diag_opendir_calls == 1U);
    assert(s_diag_readdir_calls == 3U);
    assert(s_diag_closedir_calls == 1U);
    assert(s_delay_calls == 1U);
    assert(s_last_delay_ticks == pdMS_TO_TICKS(100));
    assert(s_info_calls == 1U);
    assert(s_card_status_calls == 1U);
    assert(s_remove_calls == 1U);
    assert(strcmp(s_last_log_tag, "SD_CARD") == 0);
    assert(strstr(s_all_logs, "SD_PROBE_DIAG stat rc=-1 errno=2") != NULL);
    assert(strstr(s_all_logs, "SD_PROBE_DIAG dir rc=0 errno=0") != NULL);
    assert(strstr(s_all_logs, "SD_PROBE_DIAG entry=KEEP.TXT stat=0 errno=0 size=17") != NULL);
    assert(strstr(s_all_logs, "SD_PROBE_DIAG entry=MISSING.TMP stat=-1 errno=2") != NULL);
    assert(strstr(s_all_logs, "SD_PROBE_DIAG space err=0 capacity=33554432 total=31457280 free=12582912") != NULL);
    assert(strstr(s_all_logs, "SD_PROBE_DIAG retry_open=-1 errno=2") != NULL);
    assert(strstr(s_all_logs, "SD_PROBE_DIAG card=present mem=1 sdio=0 mmc=0 status=0 wp=not-exposed") != NULL);
    assert(strstr(s_last_log, "stage=READ") != NULL);
    assert(strstr(s_last_log, "errno=2 (") != NULL);
}

static void test_flush_close_and_read_failures_report_original_errno(void)
{
    reset_fakes();
    s_fail_flush = true;
    s_fail_close_call = 1U;
    s_fail_remove = true;
    assert_failure(SD_CARD_PROBE_STAGE_FLUSH, EIO);

    reset_fakes();
    s_fail_close_call = 1U;
    assert_failure(SD_CARD_PROBE_STAGE_CLOSE, EIO);

    reset_fakes();
    s_fail_read_open = true;
    assert_failure(SD_CARD_PROBE_STAGE_READ, EIO);

    reset_fakes();
    s_fail_read = true;
    s_fail_close_call = 2U;
    s_fail_remove = true;
    assert_failure(SD_CARD_PROBE_STAGE_READ, EIO);

    reset_fakes();
    s_fail_close_call = 2U;
    assert_failure(SD_CARD_PROBE_STAGE_CLOSE, EIO);
}

static void test_cleanup_does_not_replace_primary_error_or_invent_errno(void)
{
    reset_fakes();
    s_fail_write = true;
    s_fail_close_call = 1U;
    s_fail_remove = true;
    assert_failure(SD_CARD_PROBE_STAGE_WRITE, ENOSPC);

    reset_fakes();
    s_fail_create = true;
    s_fail_remove = true;
    assert_failure(SD_CARD_PROBE_STAGE_CREATE, EACCES);

    reset_fakes();
    s_fail_write = true;
    s_write_errno = 0;
    s_fail_close_call = 1U;
    assert_failure(SD_CARD_PROBE_STAGE_WRITE, 0);
    assert(strstr(s_last_log, "not supplied") != NULL);

    reset_fakes();
    s_short_read = true;
    s_fail_remove = true;
    assert_failure(SD_CARD_PROBE_STAGE_COMPARE, 0);

    reset_fakes();
    s_corrupt_read = true;
    assert_failure(SD_CARD_PROBE_STAGE_COMPARE, 0);

    reset_fakes();
    s_fail_remove = true;
    assert_failure(SD_CARD_PROBE_STAGE_REMOVE, EACCES);
}

static void test_diagnostic_paths_are_valid_short_fat_names(void)
{
    const char *paths[] = {BOOT_PROBE_PATH, TEST_FILE_PATH};
    for (size_t i = 0U; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        const char *name = strrchr(paths[i], '/') + 1;
        const char *dot = strchr(name, '.');
        assert(dot != NULL && dot > name && dot - name <= 8);
        assert(strlen(dot + 1) > 0U && strlen(dot + 1) <= 3U);
        assert(strchr(dot + 1, '.') == NULL);
        for (const char *character = name; *character != '\0'; ++character) {
            assert((*character >= 'A' && *character <= 'Z') || *character == '.');
        }
    }
}


int main(void)
{
    test_probe_write_read_compare_and_remove();
    test_unmounted_card_is_typed_failure_without_io();
    test_create_and_write_failures_clean_temporary_file();
    test_compare_and_remove_failures_are_reported();
    test_flush_close_and_read_failures_report_original_errno();
    test_cleanup_does_not_replace_primary_error_or_invent_errno();
    test_read_open_enoent_runs_bounded_diagnostics_without_changing_result();
    test_diagnostic_paths_are_valid_short_fat_names();
    puts("SD_CARD_PROBE_HOST_PASS");
    return 0;
}
