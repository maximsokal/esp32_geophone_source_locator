#include "ui_internal.h"

#include <inttypes.h>
#include <stdio.h>

#include "esp_timer.h"


#define UI_MEAS_COLOR_ACCENT  0x21C7D9
#define UI_MEAS_COLOR_GOOD    0x4CD681
#define UI_MEAS_COLOR_MUTED   0x94A7BC
#define UI_MEAS_COLOR_TEXT    0xF3F7FB
#define UI_MEAS_COLOR_ERROR   0xF05A67


static lv_obj_t *ui_measurements_label(
    lv_obj_t *parent,
    int x,
    int y,
    int width,
    const char *text,
    uint32_t color
)
{
    lv_obj_t *label =
        lv_label_create(
            parent
        );

    lv_obj_set_pos(
        label,
        x,
        y
    );

    lv_obj_set_width(
        label,
        width
    );

    lv_label_set_long_mode(
        label,
        LV_LABEL_LONG_CLIP
    );

    lv_label_set_text(
        label,
        text
    );

    lv_obj_set_style_text_color(
        label,
        lv_color_hex(color),
        0
    );

    return label;
}


void ui_render_measurements_locked(void)
{
    ui_context_t *context =
        ui_context();

    lv_obj_t *screen =
        ui_create_screen_base_locked(
            "MEASUREMENTS",
            "LEFT BACK   LIVE ADC STATUS"
        );

    if (screen == NULL) {
        return;
    }

    lv_obj_t *summary_card =
        ui_create_card_locked(
            screen,
            10,
            42,
            300,
            44
        );

    context->measurement_summary =
        ui_measurements_label(
            summary_card,
            10,
            7,
            280,
            "ACQUISITION --",
            UI_MEAS_COLOR_TEXT
        );

    context->measurement_health =
        ui_measurements_label(
            summary_card,
            10,
            24,
            280,
            "SAMPLES --  ERRORS --",
            UI_MEAS_COLOR_MUTED
        );

    lv_obj_t *transport_card =
        ui_create_card_locked(
            screen,
            10,
            92,
            300,
            34
        );

    context->measurement_transport =
        ui_measurements_label(
            transport_card,
            10,
            9,
            280,
            "TRANSPORT --",
            UI_MEAS_COLOR_ACCENT
        );

    ui_measurements_label(
        screen,
        12,
        132,
        296,
        "LIVE INPUTS",
        UI_MEAS_COLOR_MUTED
    );

    for (
        size_t channel = 0;
        channel < SEISMIC_ACQUISITION_MAX_CHANNELS;
        ++channel
    ) {
        const int column =
            channel >= 4
                ? 1
                : 0;

        const int row =
            (int)(channel % 4U);

        const int x =
            column == 0
                ? 12
                : 164;

        const int y =
            151 +
            (row * 15);

        context->measurement_channel_labels[channel] =
            ui_measurements_label(
                screen,
                x,
                y,
                144,
                "V-  --",
                UI_MEAS_COLOR_TEXT
            );
    }

    ui_measurements_refresh_locked();
}


void ui_measurements_refresh_locked(void)
{
    ui_context_t *context =
        ui_context();

    if (
        context->measurement_summary == NULL ||
        context->measurement_transport == NULL ||
        context->measurement_health == NULL
    ) {
        return;
    }

    seismic_acquisition_snapshot_t snapshot;

    esp_err_t err =
        seismic_acquisition_get_snapshot(
            &snapshot
        );

    if (err != ESP_OK) {
        lv_label_set_text(
            context->measurement_summary,
            "ACQUISITION STATUS UNAVAILABLE"
        );

        lv_obj_set_style_text_color(
            context->measurement_summary,
            lv_color_hex(UI_MEAS_COLOR_ERROR),
            0
        );

        return;
    }

    char text[96];

    snprintf(
        text,
        sizeof(text),
        "%s  %s  %luHz  %uCH  C%d",
        snapshot.source_name,
        snapshot.running
            ? "RUN"
            : "STOP",
        (unsigned long)snapshot.sample_rate_hz,
        (unsigned)snapshot.channel_count,
        snapshot.core_id
    );

    lv_label_set_text(
        context->measurement_summary,
        text
    );

    lv_obj_set_style_text_color(
        context->measurement_summary,
        lv_color_hex(
            snapshot.running
                ? UI_MEAS_COLOR_GOOD
                : UI_MEAS_COLOR_ERROR
        ),
        0
    );

    if (
        snapshot.transport_name != NULL &&
        snapshot.transport_name[0] != '\0'
    ) {
        if (snapshot.bus_clock_hz > 0) {
            snprintf(
                text,
                sizeof(text),
                "%s M%u %.1fMHz  +/-%.0fV  OSx%u",
                snapshot.transport_name,
                (unsigned)snapshot.bus_mode,
                (double)snapshot.bus_clock_hz /
                    1000000.0,
                (double)snapshot.input_range_v,
                (unsigned)snapshot.oversampling_ratio
            );
        } else {
            snprintf(
                text,
                sizeof(text),
                "%s",
                snapshot.transport_name
            );
        }
    } else {
        snprintf(
            text,
            sizeof(text),
            "SOFTWARE SOURCE"
        );
    }

    lv_label_set_text(
        context->measurement_transport,
        text
    );

    int64_t age_ms =
        -1;

    if (snapshot.last_sample_timestamp_us > 0) {
        age_ms =
            (esp_timer_get_time() -
             snapshot.last_sample_timestamp_us) /
            1000;
    }

    snprintf(
        text,
        sizeof(text),
        "SAMPLES %" PRIu64 "  ERR %" PRIu64 "  AGE %" PRId64 "ms",
        snapshot.samples,
        snapshot.read_errors,
        age_ms
    );

    lv_label_set_text(
        context->measurement_health,
        text
    );

    for (
        size_t channel = 0;
        channel < SEISMIC_ACQUISITION_MAX_CHANNELS;
        ++channel
    ) {
        lv_obj_t *label =
            context->measurement_channel_labels[channel];

        if (label == NULL) {
            continue;
        }

        if (
            snapshot.latest_valid &&
            channel < snapshot.channel_count
        ) {
            snprintf(
                text,
                sizeof(text),
                "V%u  %8.3f mV",
                (unsigned)(channel + 1U),
                (double)snapshot.latest_uv[channel] /
                    1000.0
            );
        } else {
            snprintf(
                text,
                sizeof(text),
                "V%u  --",
                (unsigned)(channel + 1U)
            );
        }

        lv_label_set_text(
            label,
            text
        );
    }
}
