#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "ui_i18n.h"
#include "ui_measurements_nodes.h"
#include "ui_utf8_format.h"

#define TEST_OUTPUT_CAP 256U
#define TEST_GUARD 0xA5U

typedef enum {
    FORMAT_ERROR_RETRY,
    FORMAT_CALIBRATION_HEADER,
    FORMAT_CALIBRATION_ROW,
    FORMAT_PLACEMENT_ROW,
    FORMAT_RANGING_PAIRS,
    FORMAT_SCAN_VOXELS,
} format_case_t;

static int format_case(format_case_t which, char *dst, size_t cap)
{
    switch (which) {
        case FORMAT_ERROR_RETRY:
            return ui_utf8_format(dst, cap, ui_tr(UI_I18N_KEY_MEASUREMENT_ERROR_RETRY), "SD CARD");
        case FORMAT_CALIBRATION_HEADER:
            return ui_utf8_format(dst, cap, ui_tr(UI_I18N_KEY_TFT_WORKFLOW_CALIBRATION_HEADER),
                "12", "8", "10", "25");
        case FORMAT_CALIBRATION_ROW:
            return ui_utf8_format(dst, cap, ui_tr(UI_I18N_KEY_TFT_WORKFLOW_CALIBRATION_ROW),
                "G4", "1.2", "-50", "4.7", "0.3", "12");
        case FORMAT_PLACEMENT_ROW:
            return ui_utf8_format(dst, cap, ui_tr(UI_I18N_KEY_TFT_WORKFLOW_PLACEMENT_ROW),
                "G4", "1250", "-25", "3.4", "MAIN");
        case FORMAT_RANGING_PAIRS:
            return ui_utf8_format(dst, cap, ui_tr(UI_I18N_KEY_TFT_WORKFLOW_RANGING_PAIRS),
                "5", "10", "123");
        case FORMAT_SCAN_VOXELS:
            return ui_utf8_format(dst, cap, ui_tr(UI_I18N_KEY_SCAN_VOXELS),
                123456U, 12U, "  ", ui_tr(UI_I18N_KEY_SHARED_EMPTY));
        default:
            return -1;
    }
}

static bool valid_utf8(const unsigned char *text, size_t length)
{
    size_t offset = 0U;
    while (offset < length) {
        const unsigned char lead = text[offset];
        size_t width = 0U;
        if (lead <= 0x7FU) {
            ++offset;
            continue;
        } else if (lead >= 0xC2U && lead <= 0xDFU) {
            width = 2U;
        } else if (lead >= 0xE0U && lead <= 0xEFU) {
            width = 3U;
        } else if (lead >= 0xF0U && lead <= 0xF4U) {
            width = 4U;
        } else {
            return false;
        }

        if (length - offset < width) return false;
        const unsigned char second = text[offset + 1U];
        if ((second & 0xC0U) != 0x80U ||
            (width >= 3U && (text[offset + 2U] & 0xC0U) != 0x80U) ||
            (width == 4U && (text[offset + 3U] & 0xC0U) != 0x80U) ||
            (lead == 0xE0U && second < 0xA0U) ||
            (lead == 0xEDU && second >= 0xA0U) ||
            (lead == 0xF0U && second < 0x90U) ||
            (lead == 0xF4U && second >= 0x90U)) {
            return false;
        }
        offset += width;
    }
    return true;
}

static size_t bounded_length(const char *text, size_t cap)
{
    size_t length = 0U;
    while (length < cap && text[length] != '\0') ++length;
    return length;
}

static void check_capacity(format_case_t which)
{
    char full[TEST_OUTPUT_CAP];
    const int full_result = format_case(which, full, sizeof(full));
    assert(full_result >= 0 && (size_t)full_result < sizeof(full));
    const size_t full_length = (size_t)full_result;
    assert(strlen(full) == full_length);
    assert(valid_utf8((const unsigned char *)full, full_length));

    for (size_t cap = 1U; cap <= full_length + 2U; ++cap) {
        unsigned char guarded[TEST_OUTPUT_CAP + 4U];
        memset(guarded, TEST_GUARD, sizeof(guarded));
        char *const output = (char *)&guarded[1];
        const int result = format_case(which, output, cap);
        assert(result == full_result);
        assert(guarded[0] == TEST_GUARD && guarded[cap + 1U] == TEST_GUARD);

        const size_t output_length = bounded_length(output, cap);
        assert(output_length < cap);
        assert(valid_utf8((const unsigned char *)output, output_length));
        assert(output_length <= full_length);
        assert(memcmp(output, full, output_length) == 0);
        if (cap <= full_length) assert(output_length < full_length);
        else assert(output_length == full_length);
    }
}

static int format_measurement_node(char *dst, size_t cap, const ui_measurements_node_text_t *text)
{
    return ui_measurements_node_format(
        dst, cap, ui_tr(UI_I18N_KEY_MEASUREMENT_NODE_MAIN), false, 0.0f, 0.0f,
        UI_MEASUREMENTS_NODE_LEVEL_NO_DATA, text);
}

static void test_measurement_node_call_site(void)
{
    const ui_measurements_node_text_t text = {
        .unknown = ui_tr(UI_I18N_KEY_MEASUREMENT_UNKNOWN),
        .no_data = ui_tr(UI_I18N_KEY_MEASUREMENT_NODE_NO_DATA),
        .leveled = ui_tr(UI_I18N_KEY_MEASUREMENT_NODE_LEVEL),
        .tilted = ui_tr(UI_I18N_KEY_MEASUREMENT_NODE_TILTED),
        .moving = ui_tr(UI_I18N_KEY_MEASUREMENT_NODE_MOVING),
        .solved = ui_tr(UI_I18N_KEY_MEASUREMENT_NODE_SOLVED),
        .unsolved = ui_tr(UI_I18N_KEY_MEASUREMENT_NODE_UNSOLVED),
    };
    char full[TEST_OUTPUT_CAP];
    const int full_result = format_measurement_node(full, sizeof(full), &text);
    assert(full_result >= 0 && (size_t)full_result < sizeof(full));
    const size_t full_length = (size_t)full_result;
    assert(valid_utf8((const unsigned char *)full, full_length));

    for (size_t cap = 1U; cap <= full_length + 2U; ++cap) {
        unsigned char guarded[TEST_OUTPUT_CAP + 4U];
        memset(guarded, TEST_GUARD, sizeof(guarded));
        char *const output = (char *)&guarded[1];
        assert(format_measurement_node(output, cap, &text) == full_result);
        assert(guarded[0] == TEST_GUARD && guarded[cap + 1U] == TEST_GUARD);
        const size_t output_length = bounded_length(output, cap);
        assert(output_length < cap);
        assert(valid_utf8((const unsigned char *)output, output_length));
        assert(memcmp(output, full, output_length) == 0);
        if (cap <= full_length) assert(output_length < full_length);
        else assert(output_length == full_length);
    }
}

static void test_plain_snprintf_splits_a_catalog_code_point(void)
{
    const char *ukrainian = ui_tr(UI_I18N_KEY_TFT_WORKFLOW_CALIBRATION_STATUS);
    char plain[2];
    const int required = snprintf(plain, sizeof(plain), "%s", ukrainian);
    assert(required >= 2);
    assert((unsigned char)plain[0] >= 0xC2U && (unsigned char)plain[0] <= 0xF4U);
    assert(plain[1] == '\0');
    assert(!valid_utf8((const unsigned char *)plain, 1U));
}

static void test_null_and_zero_capacity(void)
{
    char untouched = 'x';
    const char *const value = ui_tr(UI_I18N_KEY_SHARED_EMPTY);
    assert(ui_utf8_format(NULL, 1U, "%s", value) == (int)strlen(value));
    assert(ui_utf8_format(&untouched, 0U, "%s", value) == (int)strlen(value));
    assert(untouched == 'x');
    assert(ui_utf8_format(NULL, 0U, "%s", value) == (int)strlen(value));
}

int main(void)
{
    ui_i18n_set_locale(UI_LOCALE_UK);
    test_plain_snprintf_splits_a_catalog_code_point();
    check_capacity(FORMAT_ERROR_RETRY);
    check_capacity(FORMAT_CALIBRATION_HEADER);
    check_capacity(FORMAT_CALIBRATION_ROW);
    check_capacity(FORMAT_PLACEMENT_ROW);
    check_capacity(FORMAT_RANGING_PAIRS);
    check_capacity(FORMAT_SCAN_VOXELS);
    test_measurement_node_call_site();
    test_null_and_zero_capacity();
    return 0;
}
