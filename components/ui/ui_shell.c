#include "ui_internal.h"
#include "ui_power_badge.h"
#include "ui_fonts.h"
#include "ui_utf8_format.h"

#include <stdio.h>
#include <string.h>

#include "wifi_manager.h"


static const char *ui_boot_stage_text(const char *stage)
{
    if (stage == NULL) return ui_tr(UI_I18N_KEY_BOOT_STAGE_STARTING);
    if (strcmp(stage, "STARTING...") == 0) return ui_tr(UI_I18N_KEY_BOOT_STAGE_STARTING);
    if (strcmp(stage, "STORAGE...") == 0) return ui_tr(UI_I18N_KEY_BOOT_STAGE_STORAGE);
    if (strcmp(stage, "ACTIVE OBJECT...") == 0) return ui_tr(UI_I18N_KEY_BOOT_STAGE_ACTIVE_OBJECT);
    if (strcmp(stage, "OBJECT STORE...") == 0) return ui_tr(UI_I18N_KEY_BOOT_STAGE_OBJECT_STORE);
    if (strcmp(stage, "NETWORK + UI...") == 0) return ui_tr(UI_I18N_KEY_BOOT_STAGE_NETWORK);
    return stage;
}


static void ui_set_label_text_if_changed(
    lv_obj_t *label,
    const char *text
)
{
    if (label == NULL || text == NULL) {
        return;
    }

    const char *current = lv_label_get_text(label);
    if (current == NULL || strcmp(current, text) != 0) {
        lv_label_set_text(label, text);
    }
}


static void ui_set_label_color_if_changed(
    lv_obj_t *label,
    uint32_t color_hex
)
{
    if (label == NULL) {
        return;
    }

    const lv_color_t color = lv_color_hex(color_hex);
    if (!lv_color_eq(lv_obj_get_style_text_color(label, 0), color)) {
        lv_obj_set_style_text_color(label, color, 0);
    }
}


static void ui_style_screen(lv_obj_t *screen)
{
    lv_obj_set_style_bg_color(screen, lv_color_hex(UI_COLOR_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_scrollable(screen, false);
}


lv_obj_t *ui_create_label_locked(
    lv_obj_t *parent,
    int x,
    int y,
    int width,
    const char *text,
    uint32_t color
)
{
    if (parent == NULL) {
        return NULL;
    }

    lv_obj_t *label = lv_label_create(parent);
    if (label == NULL) {
        return NULL;
    }

    lv_label_set_text(label, text != NULL ? text : "");
    lv_obj_set_style_text_font(label, &ui_font_montserrat_14, 0);
    lv_obj_set_pos(label, x, y);

    if (width > 0) {
        lv_obj_set_width(label, width);
        lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    }

    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}


lv_obj_t *ui_create_screen_base_locked(
    const char *title,
    const char *footer
)
{
    ui_context_t *context = ui_context();
    lv_obj_t *previous = context->root;

    context->header_source = NULL;
    context->header_wifi = NULL;
    context->header_power = NULL;
    context->footer_label = NULL;
    context->critical_bar = NULL;
    context->critical_label = NULL;
    memset(context->home_rows, 0, sizeof(context->home_rows));
    memset(
        context->measurement_channel_labels,
        0,
        sizeof(context->measurement_channel_labels)
    );
    memset(
        context->measurement_charts,
        0,
        sizeof(context->measurement_charts)
    );
    memset(
        context->measurement_min_series,
        0,
        sizeof(context->measurement_min_series)
    );
    memset(
        context->measurement_max_series,
        0,
        sizeof(context->measurement_max_series)
    );
    memset(
        context->measurement_node_labels,
        0,
        sizeof(context->measurement_node_labels)
    );
    context->measurement_summary = NULL;
    context->measurement_transport = NULL;
    context->measurement_health = NULL;
    context->measurement_screen = NULL;
    context->measurement_content = NULL;
    context->measurement_graphs = NULL;
    context->system_heap = NULL;
    context->system_psram = NULL;
    context->system_uptime = NULL;

    lv_obj_t *screen = lv_obj_create(NULL);
    if (screen == NULL) {
        return NULL;
    }

    ui_style_screen(screen);
    context->root = screen;

    lv_obj_t *header = lv_obj_create(screen);
    if (header == NULL) {
        lv_obj_delete(screen);
        context->root = previous;
        return NULL;
    }

    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, 320, 34);
    lv_obj_set_style_bg_color(header, lv_color_hex(UI_COLOR_HEADER), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);

    (void)ui_create_label_locked(
        header, 10, 9, 112, title, UI_COLOR_TEXT
    );

    context->header_source = ui_create_label_locked(
        header, 124, 9, 76, ui_tr(UI_I18N_KEY_HEADER_ADC_IDLE), UI_COLOR_ACCENT
    );

    /* MAX-182: power badge on every screen (icon + about 6 characters). */
    context->header_power = ui_create_label_locked(
        header, 200, 9, 58, ui_tr(UI_I18N_KEY_HEADER_POWER_EMPTY), UI_COLOR_MUTED
    );

    context->header_wifi = ui_create_label_locked(
        header, 258, 9, 56, ui_tr(UI_I18N_KEY_HEADER_NETWORK_OFFLINE), UI_COLOR_MUTED
    );

    if (context->header_wifi != NULL) {
        lv_obj_set_style_text_align(
            context->header_wifi,
            LV_TEXT_ALIGN_RIGHT,
            0
        );
    }

    lv_obj_t *footer_bar = lv_obj_create(screen);
    if (footer_bar != NULL) {
        lv_obj_set_pos(footer_bar, 0, 216);
        lv_obj_set_size(footer_bar, 320, 24);
        lv_obj_set_style_bg_color(
            footer_bar,
            lv_color_hex(UI_COLOR_HEADER),
            0
        );
        lv_obj_set_style_bg_opa(footer_bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(footer_bar, 0, 0);
        lv_obj_set_style_radius(footer_bar, 0, 0);
        lv_obj_set_style_pad_all(footer_bar, 0, 0);

        context->footer_label = ui_create_label_locked(
            footer_bar,
            8,
            (24 - (int)ui_font_montserrat_14.line_height) / 2,
            304,
            footer,
            UI_COLOR_MUTED
        );
    }

    /* Above the footer, hidden until the battery is critical (see ui_refresh_critical_bar_locked). */
    context->critical_bar = lv_obj_create(screen);
    if (context->critical_bar != NULL) {
        lv_obj_set_pos(context->critical_bar, 0, 216);
        lv_obj_set_size(context->critical_bar, 320, 24);
        lv_obj_set_style_bg_color(context->critical_bar, lv_color_hex(UI_COLOR_ERROR), 0);
        lv_obj_set_style_bg_opa(context->critical_bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(context->critical_bar, 0, 0);
        lv_obj_set_style_radius(context->critical_bar, 0, 0);
        lv_obj_set_style_pad_all(context->critical_bar, 0, 0);
        lv_obj_set_hidden(context->critical_bar, true);
        context->critical_label = ui_create_label_locked(
            context->critical_bar,
            8,
            (24 - (int)ui_font_montserrat_14.line_height) / 2,
            304,
            "",
            UI_COLOR_HEADER
        );
    }

    ui_refresh_header_locked();
    lv_screen_load(screen);

    if (
        previous != NULL &&
        previous != screen &&
        lv_obj_is_valid(previous)
    ) {
        lv_obj_delete(previous);
    }

    return screen;
}


lv_obj_t *ui_create_card_locked(
    lv_obj_t *parent,
    int x,
    int y,
    int width,
    int height
)
{
    if (parent == NULL) {
        return NULL;
    }

    lv_obj_t *card = lv_obj_create(parent);
    if (card == NULL) {
        return NULL;
    }

    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, width, height);
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(
        card,
        lv_color_hex(UI_COLOR_PANEL_ALT),
        0
    );
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 7, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_scrollable(card, false);

    return card;
}


lv_obj_t *ui_create_row_locked(
    lv_obj_t *parent,
    int y,
    const char *text,
    bool selected,
    bool enabled
)
{
    if (parent == NULL) return NULL;
    lv_obj_update_layout(parent);
    const int width = lv_obj_get_content_width(parent) - 20;
    if (width <= 42) return NULL;
    lv_obj_t *row = ui_create_card_locked(parent, 10, y, width, UI_MENU_ROW_HEIGHT);
    if (row == NULL) {
        return NULL;
    }

    if (selected) {
        lv_obj_set_style_bg_color(
            row,
            lv_color_hex(UI_COLOR_PANEL_ALT),
            0
        );
        lv_obj_set_style_border_color(
            row,
            lv_color_hex(
                enabled
                    ? UI_COLOR_ACCENT
                    : UI_COLOR_MUTED
            ),
            0
        );
        lv_obj_set_style_border_width(row, 2, 0);
    }

    (void)ui_create_label_locked(
        row,
        10,
        UI_MENU_LABEL_Y,
        width - 42,
        text,
        enabled && selected
            ? UI_COLOR_TEXT
            : UI_COLOR_MUTED
    );

    if (selected && enabled) {
        (void)ui_create_label_locked(
            row, width - 22, UI_MENU_LABEL_Y, 12, ">", UI_COLOR_ACCENT
        );
    }

    return row;
}


void ui_set_notice_locked(const char *text)
{
    ui_context_t *context = ui_context();

    if (text == NULL) {
        context->notice[0] = '\0';
        return;
    }

    ui_utf8_format(context->notice, sizeof(context->notice), "%s", text);
}


static const char *ui_power_icon_symbol(ui_power_icon_t icon)
{
    switch (icon) {
        case UI_POWER_ICON_USB: return LV_SYMBOL_USB;
        case UI_POWER_ICON_CHARGE: return LV_SYMBOL_CHARGE;
        case UI_POWER_ICON_BATTERY_FULL: return LV_SYMBOL_BATTERY_FULL;
        case UI_POWER_ICON_BATTERY_3: return LV_SYMBOL_BATTERY_3;
        case UI_POWER_ICON_BATTERY_2: return LV_SYMBOL_BATTERY_2;
        case UI_POWER_ICON_BATTERY_1: return LV_SYMBOL_BATTERY_1;
        case UI_POWER_ICON_BATTERY_EMPTY: return LV_SYMBOL_BATTERY_EMPTY;
        case UI_POWER_ICON_NONE:
        default: return "";
    }
}


static uint32_t ui_power_severity_color(ui_power_severity_t severity)
{
    switch (severity) {
        case UI_POWER_SEVERITY_GOOD: return UI_COLOR_GOOD;
        case UI_POWER_SEVERITY_NORMAL: return UI_COLOR_TEXT;
        case UI_POWER_SEVERITY_WARNING: return UI_COLOR_WARNING;
        case UI_POWER_SEVERITY_ALERT: return UI_COLOR_ERROR;
        case UI_POWER_SEVERITY_MUTED:
        default: return UI_COLOR_MUTED;
    }
}


static void ui_refresh_critical_bar_locked(ui_context_t *context, const device_power_status_t *power)
{
    if (context->critical_bar == NULL || context->critical_label == NULL) {
        return;
    }
    ui_power_banner_t banner;
    ui_power_banner_from_status(power, &banner);
    if (!banner.visible) {
        lv_obj_set_hidden(context->critical_bar, true);
        return;
    }
    ui_set_label_text_if_changed(context->critical_label, banner.text);
    lv_obj_set_hidden(context->critical_bar, false);
    /* Screens add their widgets after the base: keep the alarm on top of them. */
    lv_obj_move_foreground(context->critical_bar);
}


/*
 * The status read below never waits for the mutex, so it fails whenever another task holds it. A
 * critical battery (countdown or stop) that was read a moment ago stays on the screen through
 * such a miss; only a later successful read that says otherwise takes it down. Without a
 * critical state on record a miss shows nothing, as before.
 */
static device_power_status_t s_last_critical_power;
static bool s_last_critical_power_valid;


static void ui_refresh_power_badge_locked(ui_context_t *context)
{
    if (context->header_power == NULL) {
        return;
    }
    /* This runs on LVGL; never wait for the shared status mutex here. */
    static device_status_snapshot_t snapshot;
    ui_power_badge_t badge;
    const device_power_status_t *power = NULL;
    if (device_status_try_get_snapshot(&snapshot) == ESP_OK) {
        power = &snapshot.power;
        s_last_critical_power_valid =
            power->low_battery == DEVICE_LOW_BATTERY_GRACE || power->low_battery == DEVICE_LOW_BATTERY_SHUTDOWN;
        if (s_last_critical_power_valid) {
            s_last_critical_power = *power;
        }
    } else if (s_last_critical_power_valid) {
        power = &s_last_critical_power;
    }
    ui_power_badge_from_status(power, &badge);
    ui_refresh_critical_bar_locked(context, power);
    char text[UI_POWER_BADGE_TEXT_MAX + 8];
    const char *symbol = ui_power_icon_symbol(badge.icon);
    ui_utf8_format(text, sizeof(text), ui_tr(UI_I18N_KEY_HEADER_POWER_FORMAT),
             symbol, symbol[0] != '\0' ? " " : "", badge.text);
    ui_set_label_text_if_changed(context->header_power, text);
    ui_set_label_color_if_changed(context->header_power, ui_power_severity_color(badge.severity));
}


void ui_refresh_header_locked(void)
{
    ui_context_t *context = ui_context();

    if (
        context->header_source == NULL ||
        context->header_wifi == NULL
    ) {
        return;
    }

    seismic_acquisition_snapshot_t acquisition;

    if (seismic_acquisition_get_snapshot(&acquisition) == ESP_OK) {
        char text[32];
        ui_utf8_format(
            text,
            sizeof(text),
            ui_tr(UI_I18N_KEY_HEADER_SOURCE_FORMAT),
            acquisition.source_name != NULL
                ? acquisition.source_name
                : "ADC",
            acquisition.running ? LV_SYMBOL_PLAY : LV_SYMBOL_STOP
        );

        ui_set_label_text_if_changed(context->header_source, text);
        ui_set_label_color_if_changed(
            context->header_source,
            acquisition.running ? UI_COLOR_GOOD : UI_COLOR_WARNING
        );
    } else {
        ui_set_label_text_if_changed(context->header_source, ui_tr(UI_I18N_KEY_HEADER_ADC_IDLE));
        ui_set_label_color_if_changed(
            context->header_source,
            UI_COLOR_MUTED
        );
    }

    ui_refresh_power_badge_locked(context);

    char ip[24] = {0};

    if (wifi_manager_is_setup_mode()) {
        ui_set_label_text_if_changed(context->header_wifi, ui_tr(UI_I18N_KEY_HEADER_NETWORK_SETUP));
        ui_set_label_color_if_changed(
            context->header_wifi,
            UI_COLOR_WARNING
        );
    } else if (
        wifi_manager_sta_ip_string(ip, sizeof(ip)) == ESP_OK &&
        ip[0] != '\0'
    ) {
        ui_set_label_text_if_changed(context->header_wifi, ui_tr(UI_I18N_KEY_HEADER_NETWORK_READY));
        ui_set_label_color_if_changed(
            context->header_wifi,
            UI_COLOR_GOOD
        );
    } else {
        ui_set_label_text_if_changed(context->header_wifi, ui_tr(UI_I18N_KEY_HEADER_NETWORK_OFFLINE));
        ui_set_label_color_if_changed(
            context->header_wifi,
            UI_COLOR_MUTED
        );
    }
}


void ui_render_boot_locked(const char *stage)
{
    lv_obj_t *screen = ui_create_screen_base_locked(
        ui_tr(UI_I18N_KEY_BOOT_TITLE),
        ui_tr(UI_I18N_KEY_BOOT_FOOTER)
    );

    if (screen == NULL) {
        return;
    }

    lv_obj_t *card = ui_create_card_locked(screen, 18, 58, 284, 116);
    if (card == NULL) {
        return;
    }

    (void)ui_create_label_locked(
        card,
        18,
        18,
        248,
        ui_tr(UI_I18N_KEY_BOOT_STARTUP),
        UI_COLOR_ACCENT
    );

    (void)ui_create_label_locked(
        card,
        18,
        51,
        248,
        ui_boot_stage_text(stage),
        UI_COLOR_TEXT
    );

    lv_obj_t *bar = lv_bar_create(card);
    if (bar == NULL) {
        return;
    }

    lv_obj_set_pos(bar, 18, 82);
    lv_obj_set_size(bar, 248, 10);
    lv_bar_set_range(bar, 0, 100);

    int value = 12;
    if (stage != NULL) {
        if (strstr(stage, "STORAGE") != NULL) {
            value = 35;
        } else if (strstr(stage, "RECOVERY") != NULL) {
            value = 55;
        } else if (strstr(stage, "OBJECT") != NULL) {
            value = 70;
        } else if (strstr(stage, "NETWORK") != NULL) {
            value = 88;
        }
    }

    lv_bar_set_value(bar, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(
        bar,
        lv_color_hex(UI_COLOR_PANEL_ALT),
        LV_PART_MAIN
    );
    lv_obj_set_style_bg_color(
        bar,
        lv_color_hex(UI_COLOR_ACCENT),
        LV_PART_INDICATOR
    );
}


void ui_render_boot_format_progress_locked(unsigned percent)
{
    if (percent > 100U) {
        percent = 100U;
    }

    lv_obj_t *screen = ui_create_screen_base_locked(
        ui_tr(UI_I18N_KEY_BOOT_RECOVERY_TITLE),
        ui_tr(UI_I18N_KEY_BOOT_FOOTER)
    );
    if (screen == NULL) {
        return;
    }

    lv_obj_t *card = ui_create_card_locked(screen, 18, 48, 284, 148);
    if (card == NULL) {
        return;
    }

    (void)ui_create_label_locked(
        card,
        18,
        14,
        248,
        ui_tr(UI_I18N_KEY_BOOT_STARTUP),
        UI_COLOR_ACCENT
    );
    (void)ui_create_label_locked(
        card,
        18,
        43,
        248,
        ui_tr(UI_I18N_KEY_BOOT_FORMAT_PROGRESS),
        UI_COLOR_TEXT
    );
    (void)ui_create_label_locked(
        card,
        18,
        64,
        248,
        ui_tr(UI_I18N_KEY_BOOT_FORMAT_DO_NOT_POWER_OFF),
        UI_COLOR_WARNING
    );

    lv_obj_t *bar = lv_bar_create(card);
    if (bar == NULL) {
        return;
    }

    lv_obj_set_pos(bar, 18, 91);
    lv_obj_set_size(bar, 248, 10);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, (int)percent, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(
        bar,
        lv_color_hex(UI_COLOR_PANEL_ALT),
        LV_PART_MAIN
    );
    lv_obj_set_style_bg_color(
        bar,
        lv_color_hex(UI_COLOR_ACCENT),
        LV_PART_INDICATOR
    );

    char percent_text[8];
    (void)snprintf(percent_text, sizeof(percent_text), "%u%%", percent);
    (void)ui_create_label_locked(
        card,
        230,
        108,
        36,
        percent_text,
        UI_COLOR_MUTED
    );
}


void ui_render_current_locked(void)
{
    ui_screen_render_locked(ui_context()->screen);
}


void ui_refresh_dynamic_locked(void)
{
    ui_refresh_header_locked();
    ui_screen_refresh_locked(ui_context()->screen);
}
