#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "ui_i18n.h"
#include "ui_measurements_nodes.h"
#include "ui_power_badge.h"
#include "ui_utf8_format.h"
#include "ui_workflow_model.h"

typedef struct {
    const char *concept_key;
    ui_i18n_key_t tft_key;
} concept_key_t;

static const concept_key_t s_concept_keys[] = {
    {"measurement.state_recording", UI_I18N_KEY_MEASUREMENT_STATE_RECORDING},
    {"measurement.state_stopping", UI_I18N_KEY_MEASUREMENT_STATE_STOPPING},
    {"power.battery", UI_I18N_KEY_POWER_BATTERY},
    {"power.alert_low", UI_I18N_KEY_POWER_ALERT_LOW},
    {"power.critical", UI_I18N_KEY_POWER_CRITICAL},
    {"measurement.state_complete", UI_I18N_KEY_MEASUREMENT_STATE_COMPLETE},
    {"scan.reason_preview_unavailable", UI_I18N_KEY_SCAN_REASON_PREVIEW_UNAVAILABLE},
};

static void die(const char *message)
{
    fprintf(stderr, "%s\n", message);
    abort();
}

static FILE *open_fixture(ui_locale_t locale)
{
    const char *path = locale == UI_LOCALE_EN
        ? "../../../components/ui_i18n/tests/fixtures/tft_semantic_en.tsv"
        : "../../../components/ui_i18n/tests/fixtures/tft_semantic_uk.tsv";
    FILE *file = fopen(path, "rb");
    if (file == NULL) die("cannot open TFT semantic fixture");
    return file;
}

static void expect_text(FILE *fixture, const char *field, const char *actual)
{
    char row[512];
    if (fgets(row, sizeof(row), fixture) == NULL) die("missing TFT fixture row");
    char *value = strchr(row, '\t');
    if (value == NULL) die("malformed TFT fixture row");
    *value++ = '\0';
    const size_t length = strlen(value);
    if (length > 0U && value[length - 1U] == '\n') value[length - 1U] = '\0';
    const size_t content_length = strlen(value);
    if (content_length > 0U && value[content_length - 1U] == '\r') value[content_length - 1U] = '\0';
    if (strcmp(row, field) != 0 || strcmp(value, actual) != 0) {
        fprintf(stderr, "fixture mismatch for %s\nexpected: %s\nactual:   %s\n", field, value, actual);
        abort();
    }
}

static void expect_fixture_end(FILE *fixture)
{
    char extra[2];
    assert(fgets(extra, sizeof(extra), fixture) == NULL);
    assert(ferror(fixture) == 0);
}

/* ponytail: mirror the static LVGL formatter; extract shared code if that logic grows. */
static void workflow_line_text(const ui_workflow_model_line_t *line, char *text, size_t capacity)
{
    const char *args[UI_WORKFLOW_MODEL_ARGS];
    assert(line != NULL && text != NULL && capacity > 0U);
    for (size_t i = 0U; i < UI_WORKFLOW_MODEL_ARGS; ++i) {
        args[i] = line->arg_keys[i] == UI_I18N_KEY_COUNT
            ? line->args[i] : ui_tr(line->arg_keys[i]);
    }
    const int result = ui_utf8_format(text, capacity, ui_tr(line->key),
        args[0], args[1], args[2], args[3], args[4], args[5]);
    assert(result >= 0 && (size_t)result < capacity);
    assert(text[0] != '\0');
}

static void workflow_fixture(
    const char *name,
    const workflow_snapshot_t *snapshot,
    const ui_workflow_ranging_t *ranging,
    const ui_workflow_model_state_t *initial_state,
    FILE *english,
    FILE *ukrainian)
{
    ui_workflow_model_t english_model;
    ui_workflow_model_state_t english_state;
    const ui_locale_t locales[] = {UI_LOCALE_EN, UI_LOCALE_UK};
    FILE *fixtures[] = {english, ukrainian};

    for (size_t locale_index = 0U; locale_index < 2U; ++locale_index) {
        ui_i18n_set_locale(locales[locale_index]);
        workflow_snapshot_t locale_snapshot = *snapshot;
        ui_workflow_model_state_t state = *initial_state;
        ui_workflow_model_t model;
        ui_workflow_model_build(&locale_snapshot, ranging, &state, 100U, &model);
        if (locale_index == 0U) {
            english_model = model;
            english_state = state;
        } else {
            assert(memcmp(&english_model, &model, sizeof(model)) == 0);
            assert(memcmp(&english_state, &state, sizeof(state)) == 0);
            assert(memcmp(snapshot, &locale_snapshot, sizeof(*snapshot)) == 0);
        }

        char field[64];
        char text[256];
        (void)snprintf(field, sizeof(field), "%s.title", name);
        expect_text(fixtures[locale_index], field, ui_tr(model.title_key));
        (void)snprintf(field, sizeof(field), "%s.hint", name);
        expect_text(fixtures[locale_index], field, ui_tr(model.hint_key));
        if (model.notice_key < UI_I18N_KEY_COUNT) {
            (void)snprintf(field, sizeof(field), "%s.notice", name);
            expect_text(fixtures[locale_index], field, ui_tr(model.notice_key));
        }
        for (size_t i = 0U; i < model.line_count; ++i) {
            workflow_line_text(&model.lines[i], text, sizeof(text));
            if (strcmp(name, "result_unavailable") == 0 && i >= 2U) {
                assert(strchr(text, '0') == NULL && text[0] != '\0');
            }
            if (strcmp(name, "result_zero") == 0 && i >= 1U) {
                assert(strstr(text, "0.0") != NULL);
            }
            (void)snprintf(field, sizeof(field), "%s.body.%u", name, (unsigned)i);
            expect_text(fixtures[locale_index], field, text);
        }
        for (size_t i = 0U; i < model.action_count; ++i) {
            (void)snprintf(field, sizeof(field), "%s.action.%u", name, (unsigned)i);
            expect_text(fixtures[locale_index], field, ui_tr(model.action_keys[i]));
        }
    }
}

static void make_workflow_case(
    const char *name,
    workflow_snapshot_t *snapshot,
    ui_workflow_model_state_t *state)
{
    memset(snapshot, 0, sizeof(*snapshot));
    memset(state, 0, sizeof(*state));
    snapshot->progress = WORKFLOW_PROGRESS_UNKNOWN;

    if (strcmp(name, "normal") == 0) {
        snapshot->step = WORKFLOW_STEP_IDLE;
    } else if (strcmp(name, "warning") == 0) {
        snapshot->step = WORKFLOW_STEP_PLACEMENT;
        snapshot->placement.per_node_count = 1U;
        (void)snprintf(snapshot->placement.per_node[0].node, sizeof(snapshot->placement.per_node[0].node), "G1");
        /* A node that is not stable always carries the reason; the screen names it above the node rows. */
        snapshot->placement.per_node[0].reason = WORKFLOW_PLACEMENT_REASON_NOT_VERTICAL;
    } else if (strcmp(name, "error") == 0) {
        snapshot->step = WORKFLOW_STEP_RESULT;
        snapshot->calculation_class = WORKFLOW_CALCULATION_CLASS_INTERNAL;
        (void)snprintf(snapshot->reason, sizeof(snapshot->reason), "calc_settings_invalid");
    } else if (strcmp(name, "result_unavailable") == 0) {
        snapshot->step = WORKFLOW_STEP_RESULT;
        state->result_data_ready = true;
    } else if (strcmp(name, "result_zero") == 0) {
        snapshot->step = WORKFLOW_STEP_RESULT;
        state->result_data_ready = true;
        state->result_waveform_available = true;
        state->result_source_valid = true;
        for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
            state->result_geometry.pods[i].computed = true;
        }
    } else if (strcmp(name, "localization_rejected") == 0) {
        snapshot->step = WORKFLOW_STEP_RESULT;
        snapshot->calculation_class = WORKFLOW_CALCULATION_CLASS_RECORD_AGAIN;
        (void)snprintf(snapshot->reason, sizeof(snapshot->reason), "localization_rejected");
    } else if (strcmp(name, "recording") == 0) {
        snapshot->step = WORKFLOW_STEP_RECORDING;
        snapshot->allowed_actions = WORKFLOW_ACTION(WORKFLOW_COMMAND_STOP_RECORDING);
        state->recording_elapsed_valid = true;
        state->recording_elapsed_ms = 65000U;
    } else if (strcmp(name, "ranging_unknown") == 0) {
        snapshot->step = WORKFLOW_STEP_RANGING;
    } else {
        die("unknown workflow fixture scenario");
    }
}

static void check_node_fixture(
    const char *field,
    bool computed,
    float x_m,
    float y_m,
    ui_measurements_node_level_t level,
    const char *node,
    FILE *english,
    FILE *ukrainian)
{
    const ui_locale_t locales[] = {UI_LOCALE_EN, UI_LOCALE_UK};
    FILE *fixtures[] = {english, ukrainian};
    for (size_t i = 0U; i < 2U; ++i) {
        ui_i18n_set_locale(locales[i]);
        const ui_measurements_node_text_t labels = {
            ui_tr(UI_I18N_KEY_MEASUREMENT_UNKNOWN),
            ui_tr(UI_I18N_KEY_MEASUREMENT_NODE_NO_DATA),
            ui_tr(UI_I18N_KEY_MEASUREMENT_NODE_LEVEL),
            ui_tr(UI_I18N_KEY_MEASUREMENT_NODE_TILTED),
            ui_tr(UI_I18N_KEY_MEASUREMENT_NODE_MOVING),
            ui_tr(UI_I18N_KEY_MEASUREMENT_NODE_SOLVED),
            ui_tr(UI_I18N_KEY_MEASUREMENT_NODE_UNSOLVED),
        };
        char text[256];
        const int result = ui_measurements_node_format(
            text, sizeof(text), node, computed, x_m, y_m, level, &labels);
        assert(result >= 0 && (size_t)result < sizeof(text));
        if (computed) {
            assert(strstr(text, "X0.0") != NULL && strstr(text, "Y0.0") != NULL);
        } else {
            assert(strstr(text, "X0.0") == NULL && strstr(text, "Y0.0") == NULL);
            assert(strstr(text, "(0,0,0)") == NULL && text[0] != '\0');
        }
        expect_text(fixtures[i], field, text);
    }
}

static device_power_status_t make_power_status(device_power_state_t state, bool percent_valid, float percent)
{
    device_power_status_t power;
    memset(&power, 0, sizeof(power));
    power.state = state;
    power.battery_percent_valid = percent_valid;
    power.battery_percent_estimate = percent;
    return power;
}

static void check_power_fixture(
    const char *field,
    const device_power_status_t *power,
    bool expect_zero_percent,
    FILE *english,
    FILE *ukrainian)
{
    const ui_locale_t locales[] = {UI_LOCALE_EN, UI_LOCALE_UK};
    FILE *fixtures[] = {english, ukrainian};
    ui_power_icon_t english_icon = UI_POWER_ICON_NONE;
    ui_power_severity_t english_severity = UI_POWER_SEVERITY_MUTED;

    for (size_t i = 0U; i < 2U; ++i) {
        ui_i18n_set_locale(locales[i]);
        ui_power_badge_t badge;
        const device_power_status_t before = *power;
        ui_power_badge_from_status(power, &badge);
        assert(memcmp(power, &before, sizeof(before)) == 0);
        assert(badge.text[0] != '\0');
        if (expect_zero_percent) {
            assert(strcmp(badge.text, "0%") == 0);
        } else if (strcmp(field, "battery_unknown.text") == 0) {
            assert(strchr(badge.text, '0') == NULL && strchr(badge.text, '%') == NULL);
        }
        if (i == 0U) {
            english_icon = badge.icon;
            english_severity = badge.severity;
        } else {
            assert(english_icon == badge.icon);
            assert(english_severity == badge.severity);
        }
        expect_text(fixtures[i], field, badge.text);
    }
}

static char *read_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) die("cannot open read-only concepts file");
    if (fseek(file, 0L, SEEK_END) != 0) die("cannot seek concepts file");
    const long size = ftell(file);
    if (size <= 0L || fseek(file, 0L, SEEK_SET) != 0) die("invalid concepts file size");
    char *text = (char *)malloc((size_t)size + 1U);
    if (text == NULL) die("cannot allocate concepts buffer");
    if (fread(text, 1U, (size_t)size, file) != (size_t)size) die("cannot read concepts file");
    text[size] = '\0';
    (void)fclose(file);
    return text;
}

static void check_concept_term(const cJSON *concepts, const concept_key_t *mapping, ui_locale_t locale)
{
    ui_i18n_set_locale(locale);
    const cJSON *concept = NULL;
    for (int i = 0; i < cJSON_GetArraySize(concepts); ++i) {
        const cJSON *candidate = cJSON_GetArrayItem(concepts, i);
        const cJSON *tft = cJSON_GetObjectItemCaseSensitive(candidate, "tft");
        if (cJSON_IsString(tft) && strcmp(tft->valuestring, mapping->concept_key) == 0) {
            concept = candidate;
            break;
        }
    }
    assert(concept != NULL);
    const cJSON *terms = cJSON_GetObjectItemCaseSensitive(concept, "equivalent_terms");
    assert(cJSON_IsArray(terms));
    const char *language = locale == UI_LOCALE_UK ? "uk" : "en";
    const char *actual = ui_tr(mapping->tft_key);
    bool matched = false;
    for (int i = 0; i < cJSON_GetArraySize(terms); ++i) {
        const cJSON *term = cJSON_GetArrayItem(terms, i);
        const cJSON *translation = cJSON_GetObjectItemCaseSensitive(term, language);
        if (cJSON_IsString(translation) && strcmp(actual, translation->valuestring) == 0) {
            matched = true;
            break;
        }
    }
    assert(matched);
}

static void test_concepts_and_locale_machine_parity(void)
{
    char *json = read_file("../../../i18n/concepts.json");
    cJSON *concepts = cJSON_Parse(json);
    free(json);
    assert(cJSON_IsArray(concepts));
    for (size_t i = 0U; i < sizeof(s_concept_keys) / sizeof(s_concept_keys[0]); ++i) {
        check_concept_term(concepts, &s_concept_keys[i], UI_LOCALE_EN);
        check_concept_term(concepts, &s_concept_keys[i], UI_LOCALE_UK);
    }
    cJSON_Delete(concepts);
}

static void test_workflow_fixtures(FILE *english, FILE *ukrainian)
{
    const char *const cases[] = {
        "normal",
        "warning",
        "error",
        "result_unavailable",
        "result_zero",
        "localization_rejected",
        "recording",
        "ranging_unknown",
    };
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        workflow_snapshot_t snapshot;
        ui_workflow_model_state_t state;
        make_workflow_case(cases[i], &snapshot, &state);
        workflow_fixture(cases[i], &snapshot, NULL, &state, english, ukrainian);
    }
}

int main(void)
{
    test_concepts_and_locale_machine_parity();

    FILE *english = open_fixture(UI_LOCALE_EN);
    FILE *ukrainian = open_fixture(UI_LOCALE_UK);
    test_workflow_fixtures(english, ukrainian);
    check_node_fixture("localization_solved.text", true, 0.0f, 0.0f,
        UI_MEASUREMENTS_NODE_LEVEL_LEVELED, "G1", english, ukrainian);
    check_node_fixture("localization_unverified.text", false, 0.0f, 0.0f,
        UI_MEASUREMENTS_NODE_LEVEL_NO_DATA, "G1", english, ukrainian);

    device_power_status_t power = make_power_status(DEVICE_POWER_STATE_BATTERY, false, 0.0f);
    check_power_fixture("battery_unknown.text", &power, false, english, ukrainian);
    power = make_power_status(DEVICE_POWER_STATE_BATTERY, true, 0.0f);
    check_power_fixture("battery_zero.text", &power, true, english, ukrainian);
    power = make_power_status(DEVICE_POWER_STATE_BATTERY_LOW, true, 8.0f);
    check_power_fixture("battery_low.text", &power, false, english, ukrainian);

    expect_fixture_end(english);
    expect_fixture_end(ukrainian);
    (void)fclose(english);
    (void)fclose(ukrainian);
    return 0;
}
