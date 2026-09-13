#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"

int fake_remove(const char *path);
#define remove fake_remove
#include "../sd_card_diagnostics.c"
#undef remove


static bool s_mounted;
static esp_err_t s_write_result;
static esp_err_t s_read_result;
static bool s_read_matches_write;
static unsigned s_write_calls;
static unsigned s_read_calls;
static unsigned s_remove_calls;
static char s_written_path[128];
static char s_written_text[256];
static char s_read_path[128];
static char s_removed_path[128];


static void reset_fakes(void)
{
    s_mounted = true;
    s_write_result = ESP_OK;
    s_read_result = ESP_OK;
    s_read_matches_write = true;
    s_write_calls = 0U;
    s_read_calls = 0U;
    s_remove_calls = 0U;
    s_written_path[0] = '\0';
    s_written_text[0] = '\0';
    s_read_path[0] = '\0';
    s_removed_path[0] = '\0';
}


const char *esp_err_to_name(esp_err_t error)
{
    (void)error;
    return "host";
}


void fake_esp_log(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
    va_list args;
    va_start(args, format);
    va_end(args);
}


bool sd_card_is_mounted(void)
{
    return s_mounted;
}


esp_err_t sd_card_diagnostics_write_file(
    const char *path,
    const char *text
)
{
    s_write_calls++;
    snprintf(s_written_path, sizeof(s_written_path), "%s", path);
    snprintf(s_written_text, sizeof(s_written_text), "%s", text);
    return s_write_result;
}


esp_err_t sd_card_diagnostics_read_file(
    const char *path,
    char *buffer,
    size_t buffer_size
)
{
    s_read_calls++;
    snprintf(s_read_path, sizeof(s_read_path), "%s", path);
    if (s_read_result != ESP_OK) {
        return s_read_result;
    }

    const char *text = s_read_matches_write
        ? s_written_text
        : "corrupt readback";
    snprintf(buffer, buffer_size, "%s", text);
    return ESP_OK;
}


int fake_remove(const char *path)
{
    s_remove_calls++;
    snprintf(s_removed_path, sizeof(s_removed_path), "%s", path);
    return 0;
}


static void test_requires_mounted_card(void)
{
    reset_fakes();
    s_mounted = false;

    assert(sd_card_diagnostics_run_write_read_test() == ESP_ERR_INVALID_STATE);
    assert(s_write_calls == 0U);
    assert(s_read_calls == 0U);
    assert(s_remove_calls == 0U);
}


static void test_write_failure_cleans_up_and_returns_error(void)
{
    reset_fakes();
    s_write_result = ESP_FAIL;

    assert(sd_card_diagnostics_run_write_read_test() == ESP_FAIL);
    assert(s_write_calls == 1U);
    assert(s_read_calls == 0U);
    assert(s_remove_calls == 1U);
    assert(strcmp(s_removed_path, TEST_FILE_PATH) == 0);
}


static void test_read_failure_cleans_up_and_returns_error(void)
{
    reset_fakes();
    s_read_result = ESP_FAIL;

    assert(sd_card_diagnostics_run_write_read_test() == ESP_FAIL);
    assert(s_write_calls == 1U);
    assert(s_read_calls == 1U);
    assert(s_remove_calls == 1U);
}


static void test_mismatch_cleans_up_and_fails_verification(void)
{
    reset_fakes();
    s_read_matches_write = false;

    assert(sd_card_diagnostics_run_write_read_test() == ESP_FAIL);
    assert(s_write_calls == 1U);
    assert(s_read_calls == 1U);
    assert(s_remove_calls == 1U);
}


static void test_success_uses_expected_path_and_payload_then_cleans_up(void)
{
    reset_fakes();

    assert(sd_card_diagnostics_run_write_read_test() == ESP_OK);
    assert(s_write_calls == 1U);
    assert(s_read_calls == 1U);
    assert(s_remove_calls == 1U);
    assert(strcmp(s_written_path, TEST_FILE_PATH) == 0);
    assert(strcmp(s_read_path, TEST_FILE_PATH) == 0);
    assert(strcmp(s_removed_path, TEST_FILE_PATH) == 0);
    assert(strcmp(s_written_text, TEST_TEXT) == 0);
}


int main(void)
{
    test_requires_mounted_card();
    test_write_failure_cleans_up_and_returns_error();
    test_read_failure_cleans_up_and_returns_error();
    test_mismatch_cleans_up_and_fails_verification();
    test_success_uses_expected_path_and_payload_then_cleans_up();
    return 0;
}
