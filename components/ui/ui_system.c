#include "ui_internal.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"


static void ui_system_label_set_if_changed(
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


void ui_render_system_locked(void)
{
    ui_context_t *context = ui_context();

    lv_obj_t *screen = ui_create_screen_base_locked(
        "SYSTEM",
        "LEFT BACK   LIVE SYSTEM STATUS"
    );

    if (screen == NULL) {
        return;
    }

    const esp_app_desc_t *app = esp_app_get_description();
    lv_obj_t *card = ui_create_card_locked(screen, 10, 42, 300, 158);

    if (card == NULL) {
        return;
    }

    char text[96];

    snprintf(
        text,
        sizeof(text),
        "FW      %s",
        app != NULL ? app->version : "--"
    );
    (void)ui_create_label_locked(
        card, 10, 10, 284, text, UI_COLOR_ACCENT
    );

    snprintf(
        text,
        sizeof(text),
        "ESP-IDF  %s",
        esp_get_idf_version()
    );
    (void)ui_create_label_locked(
        card, 10, 32, 284, text, UI_COLOR_TEXT
    );

    snprintf(
        text,
        sizeof(text),
        "LVGL     %d.%d.%d + esp_lvgl_port",
        LVGL_VERSION_MAJOR,
        LVGL_VERSION_MINOR,
        LVGL_VERSION_PATCH
    );
    (void)ui_create_label_locked(
        card, 10, 54, 284, text, UI_COLOR_GOOD
    );

    context->system_heap = ui_create_label_locked(
        card,
        10,
        76,
        284,
        "HEAP     -- KB internal free",
        UI_COLOR_TEXT
    );

    context->system_psram = ui_create_label_locked(
        card,
        10,
        98,
        284,
        "PSRAM    -- KB free",
        UI_COLOR_TEXT
    );

    context->system_uptime = ui_create_label_locked(
        card,
        10,
        120,
        284,
        "UPTIME   -- s",
        UI_COLOR_MUTED
    );

    (void)ui_create_label_locked(
        card,
        10,
        140,
        284,
        "CORE0 SERVICES/LVGL   CORE1 ACQUISITION",
        UI_COLOR_MUTED
    );

    ui_system_refresh_locked();
}


void ui_system_refresh_locked(void)
{
    ui_context_t *context = ui_context();

    if (
        context->system_heap == NULL ||
        context->system_psram == NULL ||
        context->system_uptime == NULL
    ) {
        return;
    }

    char text[96];

    snprintf(
        text,
        sizeof(text),
        "HEAP     %lu KB internal free",
        (unsigned long)(
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024U
        )
    );
    ui_system_label_set_if_changed(
        context->system_heap,
        text
    );

    snprintf(
        text,
        sizeof(text),
        "PSRAM    %lu KB free",
        (unsigned long)(
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U
        )
    );
    ui_system_label_set_if_changed(
        context->system_psram,
        text
    );

    snprintf(
        text,
        sizeof(text),
        "UPTIME   %lld s",
        (long long)(esp_timer_get_time() / 1000000LL)
    );
    ui_system_label_set_if_changed(
        context->system_uptime,
        text
    );
}
