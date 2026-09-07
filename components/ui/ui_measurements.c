#include "ui_internal.h"

#include <inttypes.h>
#include <stdio.h>


#define UI_MEASUREMENT_PREVIEW_FRAMES  512U
#define UI_MEASUREMENT_CHART_POINTS     48U
#define UI_MEASUREMENT_MIN_SCALE_UV   1000.0f


static seismic_frame_t s_preview_frames[
    UI_MEASUREMENT_PREVIEW_FRAMES
];

static int32_t s_trace[
    SEISMIC_CHANNEL_COUNT
][
    UI_MEASUREMENT_CHART_POINTS
];

static float s_scale_uv =
    UI_MEASUREMENT_MIN_SCALE_UV;


static float ui_measurements_absf(float value)
{
    return value < 0.0f ? -value : value;
}


static int32_t ui_measurements_round_to_i32(float value)
{
    if (value >= 0.0f) {
        return (int32_t)(value + 0.5f);
    }

    return (int32_t)(value - 0.5f);
}


static void ui_measurements_clear_trace(void)
{
    for (size_t channel = 0; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
        for (size_t point = 0; point < UI_MEASUREMENT_CHART_POINTS; ++point) {
            s_trace[channel][point] = 0;
        }
    }
}


static float ui_measurements_reduce_trace(
    const seismic_frame_t *frames,
    size_t frame_count
)
{
    ui_measurements_clear_trace();

    if (frames == NULL || frame_count == 0U) {
        return 0.0f;
    }

    const size_t bucket_count =
        frame_count < UI_MEASUREMENT_CHART_POINTS
            ? frame_count
            : UI_MEASUREMENT_CHART_POINTS;

    const size_t output_offset =
        UI_MEASUREMENT_CHART_POINTS - bucket_count;

    float max_abs_uv = 0.0f;

    for (size_t bucket = 0; bucket < bucket_count; ++bucket) {
        size_t start =
            (bucket * frame_count) /
            bucket_count;

        size_t end =
            ((bucket + 1U) * frame_count) /
            bucket_count;

        if (end <= start) {
            end = start + 1U;
        }

        if (end > frame_count) {
            end = frame_count;
        }

        for (size_t channel = 0; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            float selected = frames[start].channel_uv[channel];
            float selected_abs = ui_measurements_absf(selected);

            for (size_t index = start + 1U; index < end; ++index) {
                const float candidate = frames[index].channel_uv[channel];
                const float candidate_abs = ui_measurements_absf(candidate);

                if (candidate_abs > selected_abs) {
                    selected = candidate;
                    selected_abs = candidate_abs;
                }
            }

            s_trace[channel][output_offset + bucket] =
                ui_measurements_round_to_i32(selected);

            if (selected_abs > max_abs_uv) {
                max_abs_uv = selected_abs;
            }
        }
    }

    return max_abs_uv;
}


static int32_t ui_measurements_update_scale(
    float max_abs_uv
)
{
    float target = max_abs_uv * 1.25f;

    if (target < UI_MEASUREMENT_MIN_SCALE_UV) {
        target = UI_MEASUREMENT_MIN_SCALE_UV;
    }

    if (target >= s_scale_uv) {
        s_scale_uv = target;
    }
    else {
        s_scale_uv =
            (s_scale_uv * 0.95f) +
            (target * 0.05f);
    }

    int32_t scale = ui_measurements_round_to_i32(s_scale_uv);
    return scale > 0 ? scale : 1;
}


static lv_obj_t *ui_measurements_create_chart_locked(
    lv_obj_t *screen,
    size_t channel,
    int x,
    int y
)
{
    ui_context_t *context = ui_context();

    lv_obj_t *card = ui_create_card_locked(
        screen,
        x,
        y,
        148,
        55
    );

    if (card == NULL) {
        return NULL;
    }

    char label[24];
    snprintf(
        label,
        sizeof(label),
        "CH%u  --",
        (unsigned)(channel + 1U)
    );

    context->measurement_channel_labels[channel] =
        ui_create_label_locked(
            card,
            5,
            2,
            138,
            label,
            UI_COLOR_TEXT
        );

    lv_obj_t *chart = lv_chart_create(card);
    if (chart == NULL) {
        return card;
    }

    lv_obj_set_pos(chart, 4, 17);
    lv_obj_set_size(chart, 140, 34);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_pad_all(chart, 0, 0);
    lv_obj_remove_flag(chart, LV_OBJ_FLAG_SCROLLABLE);

    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, UI_MEASUREMENT_CHART_POINTS);
    lv_chart_set_div_line_count(chart, 3, 0);
    lv_chart_set_axis_range(
        chart,
        LV_CHART_AXIS_PRIMARY_Y,
        -(int32_t)UI_MEASUREMENT_MIN_SCALE_UV,
        (int32_t)UI_MEASUREMENT_MIN_SCALE_UV
    );

    context->measurement_charts[channel] = chart;
    context->measurement_series[channel] = lv_chart_add_series(
        chart,
        lv_color_hex(UI_COLOR_ACCENT),
        LV_CHART_AXIS_PRIMARY_Y
    );

    return card;
}


static void ui_measurements_refresh_charts_locked(void)
{
    ui_context_t *context = ui_context();

    size_t frame_count = 0U;
    seismic_buffer_info_t buffer_info;

    const esp_err_t err = seismic_buffer_copy_latest(
        s_preview_frames,
        UI_MEASUREMENT_PREVIEW_FRAMES,
        &frame_count,
        &buffer_info
    );

    float max_abs_uv = 0.0f;
    if (err == ESP_OK && frame_count > 0U) {
        max_abs_uv = ui_measurements_reduce_trace(
            s_preview_frames,
            frame_count
        );
    }
    else {
        ui_measurements_clear_trace();
    }

    const int32_t scale_uv =
        ui_measurements_update_scale(max_abs_uv);

    for (size_t channel = 0; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
        lv_obj_t *chart = context->measurement_charts[channel];
        lv_chart_series_t *series = context->measurement_series[channel];
        lv_obj_t *label = context->measurement_channel_labels[channel];

        if (chart != NULL && series != NULL) {
            lv_chart_set_axis_range(
                chart,
                LV_CHART_AXIS_PRIMARY_Y,
                -scale_uv,
                scale_uv
            );

            for (size_t point = 0; point < UI_MEASUREMENT_CHART_POINTS; ++point) {
                lv_chart_set_series_value_by_id(
                    chart,
                    series,
                    (uint32_t)point,
                    s_trace[channel][point]
                );
            }

            lv_chart_refresh(chart);
        }

        if (label == NULL) {
            continue;
        }

        char text[32];

        if (err == ESP_OK && frame_count > 0U) {
            const double latest_mv =
                (double)s_preview_frames[frame_count - 1U]
                    .channel_uv[channel] /
                1000.0;

            snprintf(
                text,
                sizeof(text),
                "CH%u  %+7.2f mV",
                (unsigned)(channel + 1U),
                latest_mv
            );
        }
        else {
            snprintf(
                text,
                sizeof(text),
                "CH%u  --",
                (unsigned)(channel + 1U)
            );
        }

        lv_label_set_text(label, text);
    }
}


static const char *ui_measurements_state_name(
    measurement_service_state_t state
)
{
    switch (state) {
        case MEASUREMENT_SERVICE_LIVE:
            return "LIVE";
        case MEASUREMENT_SERVICE_STARTING:
            return "STARTING";
        case MEASUREMENT_SERVICE_RECORDING:
            return "REC";
        case MEASUREMENT_SERVICE_STOPPING:
            return "STOPPING";
        case MEASUREMENT_SERVICE_REVIEW:
            return "STOPPED";
        case MEASUREMENT_SERVICE_DELETING:
            return "DELETING";
        case MEASUREMENT_SERVICE_ERROR:
        default:
            return "ERROR";
    }
}


void ui_render_measurements_locked(void)
{
    ui_context_t *context = ui_context();

    lv_obj_t *screen = ui_create_screen_base_locked(
        "MEASUREMENTS",
        "LEFT BACK   OK ACTION   UP/DOWN SELECT"
    );

    if (screen == NULL) {
        return;
    }

    lv_obj_t *summary_card = ui_create_card_locked(
        screen,
        10,
        38,
        300,
        34
    );

    if (summary_card == NULL) {
        return;
    }

    context->measurement_summary = ui_create_label_locked(
        summary_card,
        8,
        3,
        284,
        "LIVE --",
        UI_COLOR_TEXT
    );

    context->measurement_transport = ui_create_label_locked(
        summary_card,
        8,
        18,
        284,
        "OBJECT --",
        UI_COLOR_MUTED
    );

    (void)ui_measurements_create_chart_locked(screen, 0U, 10, 76);
    (void)ui_measurements_create_chart_locked(screen, 1U, 162, 76);
    (void)ui_measurements_create_chart_locked(screen, 2U, 10, 135);
    (void)ui_measurements_create_chart_locked(screen, 3U, 162, 135);

    context->measurement_health = ui_create_label_locked(
        screen,
        12,
        196,
        296,
        "OK START",
        UI_COLOR_ACCENT
    );

    ui_measurements_refresh_locked();
}


void ui_measurements_refresh_locked(void)
{
    ui_context_t *context = ui_context();

    if (
        context->measurement_summary == NULL ||
        context->measurement_transport == NULL ||
        context->measurement_health == NULL
    ) {
        return;
    }

    ui_measurements_refresh_charts_locked();

    seismic_acquisition_snapshot_t acquisition;
    const esp_err_t acquisition_err =
        seismic_acquisition_get_snapshot(&acquisition);

    measurement_service_snapshot_t measurement;
    const esp_err_t measurement_err =
        measurement_service_get_snapshot(&measurement);

    char text[128];

    if (measurement_err != ESP_OK) {
        lv_label_set_text(
            context->measurement_summary,
            "MEASUREMENT SERVICE UNAVAILABLE"
        );
        lv_obj_set_style_text_color(
            context->measurement_summary,
            lv_color_hex(UI_COLOR_ERROR),
            0
        );
        return;
    }

    const unsigned long sample_rate_hz =
        acquisition_err == ESP_OK
            ? (unsigned long)acquisition.sample_rate_hz
            : 0UL;

    if (measurement.state == MEASUREMENT_SERVICE_RECORDING) {
        snprintf(
            text,
            sizeof(text),
            "REC %s  %lus  %luHz",
            measurement.session_id[0] != '\0'
                ? measurement.session_id
                : "--",
            (unsigned long)(measurement.recording_ms / 1000U),
            sample_rate_hz
        );
    }
    else if (measurement.state == MEASUREMENT_SERVICE_REVIEW) {
        snprintf(
            text,
            sizeof(text),
            "STOPPED %s  %lus",
            measurement.session_id[0] != '\0'
                ? measurement.session_id
                : "--",
            (unsigned long)(measurement.recording_ms / 1000U)
        );
    }
    else {
        snprintf(
            text,
            sizeof(text),
            "%s  %luHz  CH1-CH4",
            ui_measurements_state_name(measurement.state),
            sample_rate_hz
        );
    }

    lv_label_set_text(context->measurement_summary, text);

    uint32_t summary_color = UI_COLOR_TEXT;
    if (measurement.state == MEASUREMENT_SERVICE_RECORDING) {
        summary_color = UI_COLOR_ERROR;
    }
    else if (
        measurement.state == MEASUREMENT_SERVICE_STARTING ||
        measurement.state == MEASUREMENT_SERVICE_STOPPING ||
        measurement.state == MEASUREMENT_SERVICE_DELETING
    ) {
        summary_color = UI_COLOR_WARNING;
    }
    else if (measurement.state == MEASUREMENT_SERVICE_ERROR) {
        summary_color = UI_COLOR_ERROR;
    }

    lv_obj_set_style_text_color(
        context->measurement_summary,
        lv_color_hex(summary_color),
        0
    );

    if (measurement.active_object_present) {
        snprintf(
            text,
            sizeof(text),
            "OBJECT %s  [%s]",
            measurement.object_name[0] != '\0'
                ? measurement.object_name
                : "--",
            measurement.object_id
        );
    }
    else {
        snprintf(text, sizeof(text), "NO ACTIVE OBJECT");
    }

    lv_label_set_text(context->measurement_transport, text);
    lv_obj_set_style_text_color(
        context->measurement_transport,
        lv_color_hex(
            measurement.active_object_present
                ? UI_COLOR_MUTED
                : UI_COLOR_ERROR
        ),
        0
    );

    uint32_t action_color = UI_COLOR_ACCENT;

    switch (measurement.state) {
        case MEASUREMENT_SERVICE_LIVE:
            snprintf(
                text,
                sizeof(text),
                "%s",
                measurement.active_object_present
                    ? "OK START"
                    : "SELECT ACTIVE OBJECT FIRST"
            );
            action_color =
                measurement.active_object_present
                    ? UI_COLOR_ACCENT
                    : UI_COLOR_ERROR;
            break;

        case MEASUREMENT_SERVICE_RECORDING:
            snprintf(
                text,
                sizeof(text),
                "OK STOP   S:%" PRIu64 "   DROP:%" PRIu64,
                measurement.samples_written,
                measurement.dropped_frames
            );
            action_color = UI_COLOR_ERROR;
            break;

        case MEASUREMENT_SERVICE_REVIEW:
            if (context->measurement_action == 0U) {
                snprintf(
                    text,
                    sizeof(text),
                    "> SAVE     DELETE   S:%" PRIu64 " D:%" PRIu64,
                    measurement.samples_written,
                    measurement.dropped_frames
                );
            }
            else {
                snprintf(
                    text,
                    sizeof(text),
                    "SAVE     > DELETE   S:%" PRIu64 " D:%" PRIu64,
                    measurement.samples_written,
                    measurement.dropped_frames
                );
            }
            action_color =
                context->measurement_action == 0U
                    ? UI_COLOR_GOOD
                    : UI_COLOR_WARNING;
            break;

        case MEASUREMENT_SERVICE_ERROR:
            snprintf(
                text,
                sizeof(text),
                "ERROR %s   OK RETRY",
                esp_err_to_name(measurement.last_error)
            );
            action_color = UI_COLOR_ERROR;
            break;

        case MEASUREMENT_SERVICE_STARTING:
        case MEASUREMENT_SERVICE_STOPPING:
        case MEASUREMENT_SERVICE_DELETING:
        default:
            snprintf(text, sizeof(text), "PLEASE WAIT...");
            action_color = UI_COLOR_WARNING;
            break;
    }

    lv_label_set_text(context->measurement_health, text);
    lv_obj_set_style_text_color(
        context->measurement_health,
        lv_color_hex(action_color),
        0
    );
}


void ui_handle_measurements_key_locked(
    keypad_key_t key
)
{
    ui_context_t *context = ui_context();

    measurement_service_snapshot_t snapshot;
    if (measurement_service_get_snapshot(&snapshot) != ESP_OK) {
        ui_set_notice_locked("MEASUREMENT SERVICE UNAVAILABLE");
        return;
    }

    const bool transitional =
        snapshot.busy ||
        snapshot.state == MEASUREMENT_SERVICE_STARTING ||
        snapshot.state == MEASUREMENT_SERVICE_STOPPING ||
        snapshot.state == MEASUREMENT_SERVICE_DELETING;

    if (transitional) {
        ui_set_notice_locked("MEASUREMENT OPERATION IN PROGRESS");
        return;
    }

    if (key == KEYPAD_KEY_LEFT) {
        if (snapshot.state == MEASUREMENT_SERVICE_RECORDING) {
            ui_set_notice_locked("STOP RECORDING BEFORE LEAVING");
            ui_measurements_refresh_locked();
            return;
        }

        if (snapshot.state == MEASUREMENT_SERVICE_REVIEW) {
            const esp_err_t err = measurement_service_keep_async();
            if (err != ESP_OK) {
                ui_set_notice_locked("FAILED TO KEEP SESSION");
                return;
            }
        }

        ui_screen_open_locked(UI_SCREEN_HOME);
        return;
    }

    if (
        snapshot.state == MEASUREMENT_SERVICE_REVIEW &&
        (key == KEYPAD_KEY_UP || key == KEYPAD_KEY_DOWN)
    ) {
        context->measurement_action =
            context->measurement_action == 0U
                ? 1U
                : 0U;
        ui_measurements_refresh_locked();
        return;
    }

    if (key != KEYPAD_KEY_OK && key != KEYPAD_KEY_RIGHT) {
        return;
    }

    esp_err_t err = ESP_ERR_INVALID_STATE;

    switch (snapshot.state) {
        case MEASUREMENT_SERVICE_LIVE:
        case MEASUREMENT_SERVICE_ERROR:
            if (!snapshot.active_object_present) {
                ui_set_notice_locked("SELECT ACTIVE OBJECT FIRST");
                ui_measurements_refresh_locked();
                return;
            }

            err = measurement_service_start_async();
            ui_set_notice_locked(
                err == ESP_OK
                    ? "STARTING RECORDING..."
                    : "START FAILED"
            );
            break;

        case MEASUREMENT_SERVICE_RECORDING:
            err = measurement_service_stop_async();
            ui_set_notice_locked(
                err == ESP_OK
                    ? "STOPPING RECORDING..."
                    : "STOP FAILED"
            );
            break;

        case MEASUREMENT_SERVICE_REVIEW:
            if (context->measurement_action == 0U) {
                err = measurement_service_keep_async();
                ui_set_notice_locked(
                    err == ESP_OK
                        ? "SESSION SAVED"
                        : "SAVE FAILED"
                );
            }
            else {
                err = measurement_service_delete_async();
                ui_set_notice_locked(
                    err == ESP_OK
                        ? "DELETING SESSION..."
                        : "DELETE FAILED"
                );
            }
            break;

        case MEASUREMENT_SERVICE_STARTING:
        case MEASUREMENT_SERVICE_STOPPING:
        case MEASUREMENT_SERVICE_DELETING:
        default:
            return;
    }

    if (err == ESP_OK) {
        ui_measurements_refresh_locked();
    }
}
