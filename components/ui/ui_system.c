#include "ui_internal.h"

#include <stdio.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"


#define UI_SYSTEM_COLOR_TEXT    0xF3F7FB
#define UI_SYSTEM_COLOR_MUTED   0x94A7BC
#define UI_SYSTEM_COLOR_ACCENT  0x21C7D9
#define UI_SYSTEM_COLOR_GOOD    0x4CD681


static void ui_system_label(
    lv_obj_t *parent,
    int x,
    int y,
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
        284
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
}


void ui_render_system_locked(void)
{
    lv_obj_t *screen =
        ui_create_screen_base_locked(
            "SYSTEM",
            "LEFT BACK"
        );

    if (screen == NULL) {
        return;
    }

    const esp_app_desc_t *app =
        esp_app_get_description();

    lv_obj_t *card =
        ui_create_card_locked(
            screen,
            10,
            42,
            300,
            158
        );

    char text[96];

    snprintf(
        text,
        sizeof(text),
        "FW      %s",
        app != NULL
            ? app->version
            : "--"
    );

    ui_system_label(
        card,
        10,
        10,
        text,
        UI_SYSTEM_COLOR_ACCENT
    );

    snprintf(
        text,
        sizeof(text),
        "ESP-IDF  %s",
        esp_get_idf_version()
    );

    ui_system_label(
        card,
        10,
        32,
        text,
        UI_SYSTEM_COLOR_TEXT
    );

    snprintf(
        text,
        sizeof(text),
        "LVGL     %d.%d.%d + esp_lvgl_port",
        LVGL_VERSION_MAJOR,
        LVGL_VERSION_MINOR,
        LVGL_VERSION_PATCH
    );

    ui_system_label(
        card,
        10,
        54,
        text,
        UI_SYSTEM_COLOR_GOOD
    );

    snprintf(
        text,
        sizeof(text),
        "HEAP     %lu KB internal free",
        (unsigned long)(
            heap_caps_get_free_size(
                MALLOC_CAP_INTERNAL
            ) /
            1024U
        )
    );

    ui_system_label(
        card,
        10,
        76,
        text,
        UI_SYSTEM_COLOR_TEXT
    );

    snprintf(
        text,
        sizeof(text),
        "PSRAM    %lu KB free",
        (unsigned long)(
            heap_caps_get_free_size(
                MALLOC_CAP_SPIRAM
            ) /
            1024U
        )
    );

    ui_system_label(
        card,
        10,
        98,
        text,
        UI_SYSTEM_COLOR_TEXT
    );

    snprintf(
        text,
        sizeof(text),
        "UPTIME   %lld s",
        (long long)(
            esp_timer_get_time() /
            1000000LL
        )
    );

    ui_system_label(
        card,
        10,
        120,
        text,
        UI_SYSTEM_COLOR_MUTED
    );

    ui_system_label(
        card,
        10,
        140,
        "CORE0 SERVICES/LVGL   CORE1 ACQUISITION",
        UI_SYSTEM_COLOR_MUTED
    );
}
