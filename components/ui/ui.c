#include "ui.h"

#include <string.h>

#include "app_event_bus.h"
#include "object_service.h"
#include "storage_service.h"
#include "ui_internal.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"


static const char *TAG =
    "UI";


static ui_context_t s_context;


ui_context_t *ui_context(void)
{
    return
        &s_context;
}


static void ui_refresh_timer_callback(
    lv_timer_t *timer
)
{
    (void)timer;

    if (!s_context.runtime_ready) {
        return;
    }

    ui_refresh_dynamic_locked();
}


esp_err_t ui_boot_init(void)
{
    memset(
        &s_context,
        0,
        sizeof(s_context)
    );

    esp_err_t err =
        ui_lvgl_init();

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "LVGL initialization failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    if (!lvgl_port_lock(1000)) {
        ui_lvgl_deinit();

        return ESP_ERR_TIMEOUT;
    }

    ui_render_boot_locked(
        "STARTING..."
    );

    lvgl_port_unlock();

    ESP_LOGI(
        TAG,
        "LVGL boot UI ready"
    );

    return ESP_OK;
}


void ui_boot_set_stage(
    const char *stage
)
{
    if (
        !s_context.port_ready ||
        stage == NULL
    ) {
        return;
    }

    if (!lvgl_port_lock(250)) {
        ESP_LOGW(
            TAG,
            "Unable to lock LVGL for boot stage '%s'",
            stage
        );

        return;
    }

    ui_render_boot_locked(
        stage
    );

    lvgl_port_unlock();
}


void ui_boot_show_error(
    const char *message
)
{
    if (
        !s_context.port_ready ||
        message == NULL
    ) {
        return;
    }

    if (!lvgl_port_lock(500)) {
        return;
    }

    ui_render_boot_error_locked(
        message
    );

    lvgl_port_unlock();
}


esp_err_t ui_init(void)
{
    if (!s_context.port_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_context.runtime_ready) {
        return ESP_OK;
    }

    esp_err_t err =
        object_service_set_event_callback(
            ui_object_service_event,
            NULL
        );

    if (err != ESP_OK) {
        return err;
    }

    err =
        storage_service_set_event_callback(
            ui_storage_service_event,
            NULL
        );

    if (err != ESP_OK) {
        return err;
    }

    s_context.runtime_ready =
        true;

    s_context.screen =
        UI_SCREEN_HOME;

    s_context.selected =
        0;

    s_context.notice[0] =
        '\0';

    if (!lvgl_port_lock(1000)) {
        s_context.runtime_ready =
            false;

        return ESP_ERR_TIMEOUT;
    }

    ui_render_current_locked();

    s_context.refresh_timer =
        lv_timer_create(
            ui_refresh_timer_callback,
            250,
            NULL
        );

    lvgl_port_unlock();

    if (s_context.refresh_timer == NULL) {
        s_context.runtime_ready =
            false;

        return ESP_ERR_NO_MEM;
    }

    err =
        ui_event_task_start();

    if (err != ESP_OK) {
        if (lvgl_port_lock(250)) {
            lv_timer_delete(
                s_context.refresh_timer
            );

            s_context.refresh_timer =
                NULL;

            lvgl_port_unlock();
        }

        s_context.runtime_ready =
            false;

        return err;
    }

    ESP_LOGI(
        TAG,
        "Interactive LVGL UI ready"
    );

    return ESP_OK;
}


void ui_shutdown(void)
{
    if (!s_context.port_ready) {
        return;
    }

    ui_event_task_stop();

    s_context.runtime_ready =
        false;

    (void)object_service_set_event_callback(
        NULL,
        NULL
    );

    (void)storage_service_set_event_callback(
        NULL,
        NULL
    );

    if (lvgl_port_lock(500)) {
        if (s_context.refresh_timer != NULL) {
            lv_timer_delete(
                s_context.refresh_timer
            );

            s_context.refresh_timer =
                NULL;
        }

        if (s_context.root != NULL) {
            lv_obj_clean(
                s_context.root
            );

            lv_obj_set_style_bg_color(
                s_context.root,
                lv_color_hex(0x000000),
                0
            );
        }

        lvgl_port_unlock();
    }

    ui_lvgl_deinit();

    ESP_LOGI(
        TAG,
        "UI stopped"
    );
}


void ui_handle_keypad_event(
    keypad_key_t key,
    void *user_context
)
{
    (void)user_context;

    if (!s_context.runtime_ready) {
        return;
    }

    esp_err_t err =
        app_event_bus_publish(
            APP_EVENT_KEYPAD,
            (int32_t)key
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to queue key %s: %s",
            keypad_key_name(key),
            esp_err_to_name(err)
        );
    }
}
