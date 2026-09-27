#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "device_status.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ui_i18n.h"
#include "ui_measurements_nodes.h"

static int s_mutex_token;
static bool s_mutex_busy;
static unsigned s_take_calls;
static unsigned s_give_calls;

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return &s_mutex_token;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks)
{
    assert(semaphore == &s_mutex_token);
    assert(ticks == 0U);
    ++s_take_calls;
    return s_mutex_busy ? pdFALSE : pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    assert(semaphore == &s_mutex_token);
    ++s_give_calls;
    return pdTRUE;
}

static size_t utf8_codepoint_count(const char *text)
{
    size_t count = 0U;
    for (const unsigned char *p = (const unsigned char *)text; *p != 0U; ++p) {
        if ((*p & 0xC0U) != 0x80U) {
            ++count;
        }
    }
    return count;
}

static void assert_translation(
    ui_i18n_key_t key,
    size_t max_len,
    const char *english,
    const char *ukrainian)
{
    ui_i18n_set_locale(UI_LOCALE_EN);
    const char *text = ui_tr(key);
    assert(strcmp(text, english) == 0);
    assert(utf8_codepoint_count(text) <= max_len);

    ui_i18n_set_locale(UI_LOCALE_UK);
    text = ui_tr(key);
    assert(strcmp(text, ukrainian) == 0);
    assert(utf8_codepoint_count(text) <= max_len);
}

static ui_measurements_node_text_t translated_text(void)
{
    ui_measurements_node_text_t text = {
        .unknown = ui_tr(UI_I18N_KEY_SHARED_EMPTY),
        .no_data = ui_tr(UI_I18N_KEY_MEASUREMENT_NODE_NO_DATA),
        .leveled = ui_tr(UI_I18N_KEY_MEASUREMENT_NODE_LEVEL),
        .tilted = ui_tr(UI_I18N_KEY_MEASUREMENT_NODE_TILTED),
        .moving = ui_tr(UI_I18N_KEY_MEASUREMENT_NODE_MOVING),
        .solved = ui_tr(UI_I18N_KEY_MEASUREMENT_NODE_SOLVED),
        .unsolved = ui_tr(UI_I18N_KEY_MEASUREMENT_NODE_UNSOLVED),
    };
    return text;
}

static void assert_row(
    const char *expected,
    const char *node,
    bool computed,
    float x_m,
    float y_m,
    ui_measurements_node_level_t level)
{
    char actual[96];
    const ui_measurements_node_text_t text = translated_text();
    const int written = ui_measurements_node_format(
        actual,
        sizeof(actual),
        node,
        computed,
        x_m,
        y_m,
        level,
        &text);
    assert(written == (int)strlen(expected));
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "row mismatch: got '%s', expected '%s'\n", actual, expected);
    }
    assert(strcmp(actual, expected) == 0);
    assert(utf8_codepoint_count(actual) <= 41U);
}

static void test_catalog_labels_fit_max_len(void)
{
    assert_translation(UI_I18N_KEY_MEASUREMENT_NODE_MAIN, 4U, "MAIN", "MAIN");
    assert_translation(UI_I18N_KEY_MEASUREMENT_NODE_G1, 2U, "G1", "G1");
    assert_translation(UI_I18N_KEY_MEASUREMENT_NODE_G2, 2U, "G2", "G2");
    assert_translation(UI_I18N_KEY_MEASUREMENT_NODE_G3, 2U, "G3", "G3");
    assert_translation(UI_I18N_KEY_MEASUREMENT_NODE_G4, 2U, "G4", "G4");
    assert_translation(UI_I18N_KEY_MEASUREMENT_NODE_NO_DATA, 3U, "N/D", "Н/Д");
    assert_translation(UI_I18N_KEY_MEASUREMENT_NODE_LEVEL, 5U, "LEVEL", "РІВНО");
    assert_translation(UI_I18N_KEY_MEASUREMENT_NODE_TILTED, 5U, "TILT", "НАХИЛ");
    assert_translation(UI_I18N_KEY_MEASUREMENT_NODE_MOVING, 4U, "MOVE", "РУХ");
    assert_translation(UI_I18N_KEY_MEASUREMENT_NODE_SOLVED, 4U, "SOL", "РОЗВ");
    assert_translation(UI_I18N_KEY_MEASUREMENT_NODE_UNSOLVED, 6U, "UNS", "НЕРОЗВ");
}

static void test_rows_in_english(void)
{
    ui_i18n_set_locale(UI_LOCALE_EN);
    assert_row("MAIN X0.0m Y0.0m N/D SOL", "MAIN", true, 0.0f, 0.0f,
               UI_MEASUREMENTS_NODE_LEVEL_NO_DATA);
    assert_row("G1 X-1.3m Y2.6m LEVEL SOL", "G1", true, -1.26f, 2.56f,
               UI_MEASUREMENTS_NODE_LEVEL_LEVELED);
    assert_row("G2 X7.5m Y-8.1m TILT SOL", "G2", true, 7.54f, -8.06f,
               UI_MEASUREMENTS_NODE_LEVEL_TILTED);
    assert_row("G3 X123456.7m Y-234567.8m MOVE SOL", "G3", true,
               123456.7f, -234567.8f, UI_MEASUREMENTS_NODE_LEVEL_MOVING);
    assert_row("G4 X-2147483.5m Y-2147483.5m TILT SOL", "G4", true,
               -2147483.5f, -2147483.5f, UI_MEASUREMENTS_NODE_LEVEL_TILTED);
    assert_row("G4 X0.0m Y0.0m N/D SOL", "G4", true, 0.0f, 0.0f,
               UI_MEASUREMENTS_NODE_LEVEL_NO_DATA);
    assert_row("G4 X-- Y-- MOVE UNS", "G4", false, 123.0f, -456.0f,
               UI_MEASUREMENTS_NODE_LEVEL_MOVING);
    assert_row("G4 X-- Y-- N/D UNS", "G4", false, 0.0f, 0.0f,
               UI_MEASUREMENTS_NODE_LEVEL_NO_DATA);
}

static void test_rows_in_ukrainian(void)
{
    ui_i18n_set_locale(UI_LOCALE_UK);
    assert_row("MAIN X0.0m Y0.0m Н/Д РОЗВ", "MAIN", true, 0.0f, 0.0f,
               UI_MEASUREMENTS_NODE_LEVEL_NO_DATA);
    assert_row("G1 X-1.3m Y2.6m РІВНО РОЗВ", "G1", true, -1.26f, 2.56f,
               UI_MEASUREMENTS_NODE_LEVEL_LEVELED);
    assert_row("G2 X7.5m Y-8.1m НАХИЛ РОЗВ", "G2", true, 7.54f, -8.06f,
               UI_MEASUREMENTS_NODE_LEVEL_TILTED);
    assert_row("G3 X123456.7m Y-234567.8m РУХ РОЗВ", "G3", true,
               123456.7f, -234567.8f, UI_MEASUREMENTS_NODE_LEVEL_MOVING);
    assert_row("G4 X-2147483.5m Y-2147483.5m НАХИЛ РОЗВ", "G4", true,
               -2147483.5f, -2147483.5f, UI_MEASUREMENTS_NODE_LEVEL_TILTED);
    assert_row("G4 X0.0m Y0.0m Н/Д РОЗВ", "G4", true, 0.0f, 0.0f,
               UI_MEASUREMENTS_NODE_LEVEL_NO_DATA);
    assert_row("G4 X-- Y-- РУХ НЕРОЗВ", "G4", false, 123.0f, -456.0f,
               UI_MEASUREMENTS_NODE_LEVEL_MOVING);
    assert_row("G4 X-- Y-- Н/Д НЕРОЗВ", "G4", false, 0.0f, 0.0f,
               UI_MEASUREMENTS_NODE_LEVEL_NO_DATA);
}

static void test_nonblocking_device_status_snapshot(void)
{
    device_status_snapshot_t snapshot;
    memset(&snapshot, 0xA5, sizeof(snapshot));

    assert(device_status_try_get_snapshot(&snapshot) == ESP_ERR_INVALID_STATE);
    assert(device_status_init() == ESP_OK);
    assert(device_status_try_get_snapshot(NULL) == ESP_ERR_INVALID_ARG);

    s_mutex_busy = true;
    assert(device_status_try_get_snapshot(&snapshot) == ESP_ERR_TIMEOUT);
    assert(s_give_calls == 0U);

    s_mutex_busy = false;
    assert(device_status_try_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.mode == DEVICE_MODE_IDLE);
    assert(s_take_calls == 2U);
    assert(s_give_calls == 1U);
}

int main(void)
{
    test_catalog_labels_fit_max_len();
    test_rows_in_english();
    test_rows_in_ukrainian();
    test_nonblocking_device_status_snapshot();
    puts("ui_measurement_nodes_test: PASS");
    return 0;
}
