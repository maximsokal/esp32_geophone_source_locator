#include "ui_internal.h"

#include <stdio.h>
#include <string.h>

#include "wifi_manager.h"


static void ui_style_screen(lv_obj_t *screen)
{
    lv_obj_set_style_bg_color(screen, lv_color_hex(UI_COLOR_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(UI_COLOR_TEXT), 0);
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
    lv_obj_set_pos(label, x, y);

    if (width > 0) {
        lv_obj_set_width(label, width);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
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
    memset(context->home_rows, 0, sizeof(context->home_rows));
    memset(
        context->measurement_channel_labels,
        0,
        sizeof(context->measurement_channel_labels)
    );
    context->measurement_summary = NULL;
    context->measurement_transport = NULL;
    context->measurement_health = NULL;
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
        header, 128, 9, 112, "ADC --", UI_COLOR_ACCENT
    );

    context->header_wifi = ui_create_label_locked(
        header, 248, 9, 66, "NET --", UI_COLOR_MUTED
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

        (void)ui_create_label_locked(
            footer_bar,
            8,
            6,
            304,
            footer,
            UI_COLOR_MUTED
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
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

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
    lv_obj_t *row = ui_create_card_locked(parent, 10, y, 300, 31);
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
        8,
        258,
        text,
        enabled && selected
            ? UI_COLOR_TEXT
            : UI_COLOR_MUTED
    );

    if (selected && enabled) {
        (void)ui_create_label_locked(
            row, 278, 8, 12, ">", UI_COLOR_ACCENT
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

    snprintf(context->notice, sizeof(context->notice), "%s", text);
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
        snprintf(
            text,
            sizeof(text),
            "%s %s",
            acquisition.source_name != NULL
                ? acquisition.source_name
                : "ADC",
            acquisition.running ? "RUN" : "OFF"
        );

        lv_label_set_text(context->header_source, text);
        lv_obj_set_style_text_color(
            context->header_source,
            lv_color_hex(
                acquisition.running
                    ? UI_COLOR_GOOD
                    : UI_COLOR_WARNING
            ),
            0
        );
    } else {
        lv_label_set_text(context->header_source, "ADC --");
        lv_obj_set_style_text_color(
            context->header_source,
            lv_color_hex(UI_COLOR_MUTED),
            0
        );
    }

    char ip[24] = {0};

    if (wifi_manager_is_setup_mode()) {
        lv_label_set_text(context->header_wifi, "SETUP");
        lv_obj_set_style_text_color(
            context->header_wifi,
            lv_color_hex(UI_COLOR_WARNING),
            0
        );
    } else if (
        wifi_manager_sta_ip_string(ip, sizeof(ip)) == ESP_OK &&
        ip[0] != '\0'
    ) {
        lv_label_set_text(context->header_wifi, "NET OK");
        lv_obj_set_style_text_color(
            context->header_wifi,
            lv_color_hex(UI_COLOR_GOOD),
            0
        );
    } else {
        lv_label_set_text(context->header_wifi, "NET --");
        lv_obj_set_style_text_color(
            context->header_wifi,
            lv_color_hex(UI_COLOR_MUTED),
            0
        );
    }
}


void ui_render_boot_locked(const char *stage)
{
    lv_obj_t *screen = ui_create_screen_base_locked(
        "GEOPHONE LOCATOR",
        "ESP32-S3  |  INITIALIZING"
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
        "SYSTEM STARTUP",
        UI_COLOR_ACCENT
    );

    (void)ui_create_label_locked(
        card,
        18,
        51,
        248,
        stage,
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


void ui_render_current_locked(void)
{
    ui_screen_render_locked(ui_context()->screen);
}


void ui_refresh_dynamic_locked(void)
{
    ui_refresh_header_locked();
    ui_screen_refresh_locked(ui_context()->screen);
}
