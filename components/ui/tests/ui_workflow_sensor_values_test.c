#include <assert.h>
#include <math.h>
#include <string.h>

#include "ui_workflow_sensor_values.h"

static void test_page_alternates_every_interval(void)
{
    assert(!ui_workflow_sensor_values_page(0U));
    assert(!ui_workflow_sensor_values_page(UI_WORKFLOW_SENSOR_PAGE_MS - 1U));
    assert(ui_workflow_sensor_values_page(UI_WORKFLOW_SENSOR_PAGE_MS));
    assert(ui_workflow_sensor_values_page(2U * UI_WORKFLOW_SENSOR_PAGE_MS - 1U));
    assert(!ui_workflow_sensor_values_page(2U * UI_WORKFLOW_SENSOR_PAGE_MS));
}

static void test_failed_status_read_keeps_the_cards_for_a_bounded_time(void)
{
    /* The sensor-check cards read the shared device status without waiting. Writers of that status
     * run at priority 1 and can be preempted while holding its lock, so single reads fail; a failed
     * read used to blank every card (grey frame, "--") for that tick. */
    ui_workflow_status_hold_t hold = {0};

    /* Nothing was ever read: there is nothing to keep. */
    assert(!ui_workflow_status_hold_update(&hold, false, 5000U));

    assert(ui_workflow_status_hold_update(&hold, true, 10000U));
    /* Failed reads right after a good one keep the last snapshot... */
    assert(ui_workflow_status_hold_update(&hold, false, 10200U));
    assert(ui_workflow_status_hold_update(&hold, false, 10000U + UI_WORKFLOW_STATUS_HOLD_MS - 1U));
    /* ...but only for a bounded time, so a really dead status cannot keep a green card. */
    assert(!ui_workflow_status_hold_update(&hold, false, 10000U + UI_WORKFLOW_STATUS_HOLD_MS));
    assert(!ui_workflow_status_hold_update(&hold, false, 20000U));

    /* A good read restarts the window. */
    assert(ui_workflow_status_hold_update(&hold, true, 20100U));
    assert(ui_workflow_status_hold_update(&hold, false, 20100U + UI_WORKFLOW_STATUS_HOLD_MS - 1U));

    /* A clock that went backwards never extends the hold. */
    assert(!ui_workflow_status_hold_update(&hold, false, 100U));

    /* A missing hold object degrades to the plain read result. */
    assert(ui_workflow_status_hold_update(NULL, true, 0U));
    assert(!ui_workflow_status_hold_update(NULL, false, 0U));

    /* The hold is exact at time zero too: a good read at t=0 still counts as a read. */
    ui_workflow_status_hold_t at_zero = {0};
    assert(ui_workflow_status_hold_update(&at_zero, true, 0U));
    assert(ui_workflow_status_hold_update(&at_zero, false, UI_WORKFLOW_STATUS_HOLD_MS - 1U));
    assert(!ui_workflow_status_hold_update(&at_zero, false, UI_WORKFLOW_STATUS_HOLD_MS));
}

static void test_value_text_rounds_and_marks_missing(void)
{
    char text[16];
    ui_workflow_sensor_value_text(text, sizeof(text), true, 24.14f, 1U);
    assert(strcmp(text, "24.1") == 0);
    ui_workflow_sensor_value_text(text, sizeof(text), true, -5.5f, 1U);
    assert(strcmp(text, "-5.5") == 0);
    ui_workflow_sensor_value_text(text, sizeof(text), false, 24.0f, 1U);
    assert(strcmp(text, "--") == 0);
    ui_workflow_sensor_value_text(text, sizeof(text), true, NAN, 1U);
    assert(strcmp(text, "--") == 0);
    ui_workflow_sensor_value_text(text, sizeof(text), true, INFINITY, 1U);
    assert(strcmp(text, "--") == 0);
}

static void test_tiny_negative_value_never_prints_minus_zero(void)
{
    char text[16];
    ui_workflow_sensor_value_text(text, sizeof(text), true, -0.04f, 1U);
    assert(strcmp(text, "0.0") == 0);
    ui_workflow_sensor_value_text(text, sizeof(text), true, -0.0f, 1U);
    assert(strcmp(text, "0.0") == 0);
    ui_workflow_sensor_value_text(text, sizeof(text), true, -0.06f, 1U);
    assert(strcmp(text, "-0.1") == 0);
}

static void test_pressure_is_shown_in_hpa(void)
{
    char text[16];
    ui_workflow_sensor_pressure_text(text, sizeof(text), true, 101325.0f);
    assert(strcmp(text, "1013.2") == 0 || strcmp(text, "1013.3") == 0);
    ui_workflow_sensor_pressure_text(text, sizeof(text), true, 100000.0f);
    assert(strcmp(text, "1000.0") == 0);
    ui_workflow_sensor_pressure_text(text, sizeof(text), false, 100000.0f);
    assert(strcmp(text, "--") == 0);
}

static void test_card_line_has_node_pressure_temperature_gyro(void)
{
    char line[40];
    ui_workflow_sensor_values_line(
        line, sizeof(line), "G1", true, 101010.0f, true, 24.14f, true, 0.26f);
    assert(strcmp(line, "G1 1010.1 24.1 0.3") == 0);

    ui_workflow_sensor_values_line(
        line, sizeof(line), "G4", true, 100000.0f, false, 0.0f, false, 0.0f);
    assert(strcmp(line, "G4 1000.0 -- --") == 0);

    ui_workflow_sensor_values_line(
        line, sizeof(line), "G2", false, 0.0f, false, 0.0f, false, 0.0f);
    assert(strcmp(line, "G2 -- -- --") == 0);
}

static void test_card_line_is_truncated_safely(void)
{
    char line[8];
    memset(line, 'x', sizeof(line));
    ui_workflow_sensor_values_line(
        line, sizeof(line), "G1", true, 101010.0f, true, 24.14f, true, 0.26f);
    assert(line[sizeof(line) - 1U] == '\0');
    assert(strncmp(line, "G1 1010", 7) == 0);
    ui_workflow_sensor_values_line(NULL, 0U, "G1", true, 1.0f, true, 1.0f, true, 1.0f);
}

int main(void)
{
    test_page_alternates_every_interval();
    test_failed_status_read_keeps_the_cards_for_a_bounded_time();
    test_value_text_rounds_and_marks_missing();
    test_tiny_negative_value_never_prints_minus_zero();
    test_pressure_is_shown_in_hpa();
    test_card_line_has_node_pressure_temperature_gyro();
    test_card_line_is_truncated_safely();
    return 0;
}
