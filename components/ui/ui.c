#include "ui.h"

#include <string.h>

#include "app_events.h"
#include "object_service.h"
#include "storage_service.h"
#include "seismic_acquisition.h"
#include "ui_frame_scheduler.h"
#include "ui_internal.h"
#include "ui_i18n_nvs.h"
#include "ui_perf_policy.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"


static const char *TAG = "UI";

static ui_context_t s_context;
static ui_frame_scheduler_t s_frame_scheduler;
static uint64_t s_refresh_generation;
static int64_t s_perf_started_us;
static uint64_t s_render_total_us;
static uint64_t s_render_max_us;
static ui_perf_policy_state_t s_perf_log_state;


ui_context_t *ui_context(void)
{
    return &s_context;
}


static void ui_refresh_timer_callback(lv_timer_t *timer)
{
    (void)timer;

    if (
        !atomic_load_explicit(
            &s_context.runtime_ready,
            memory_order_acquire
        )
    ) {
        return;
    }

    /* APP_LOOP only queues events; UI mutation stays on LVGL's own task. */
    ui_event_dispatcher_dispatch_pending_locked();

    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    seismic_acquisition_snapshot_t acquisition;
    const uint64_t generation = seismic_acquisition_get_snapshot(&acquisition) == ESP_OK
        ? acquisition.samples
        : ++s_refresh_generation;
    ui_frame_scheduler_submit(&s_frame_scheduler, generation);
    if (!ui_frame_scheduler_take(&s_frame_scheduler, now_us, NULL)) return;

    const int64_t started_us = esp_timer_get_time();
    ui_refresh_dynamic_locked();
    const uint64_t render_us = (uint64_t)(esp_timer_get_time() - started_us);
    s_render_total_us += render_us;
    if (render_us > s_render_max_us) s_render_max_us = render_us;

    if ((int64_t)now_us - s_perf_started_us >= 10000000LL) {
        const uint64_t frames = s_frame_scheduler.frames_rendered;
        const uint64_t elapsed_us = now_us - (uint64_t)s_perf_started_us;
        if (ui_perf_policy_should_log(
                &s_perf_log_state,
                (uint32_t)(now_us / 1000U),
                frames,
                elapsed_us,
                UI_FRAME_TARGET_FPS,
                s_render_max_us
            )) {
            ESP_LOGI(TAG,
                     "UI_PERF: target_fps=%u actual_fps=%.1f frames_rendered=%llu frames_coalesced=%llu max_render_us=%llu avg_render_us=%llu",
                     (unsigned)UI_FRAME_TARGET_FPS,
                     elapsed_us > 0U ? (double)frames * 1000000.0 / (double)elapsed_us : 0.0,
                     (unsigned long long)frames,
                     (unsigned long long)s_frame_scheduler.frames_coalesced,
                     (unsigned long long)s_render_max_us,
                     (unsigned long long)(frames > 0U ? s_render_total_us / frames : 0U));
        }
        ui_frame_scheduler_init(&s_frame_scheduler, now_us + UI_FRAME_MIN_PERIOD_US);
        s_perf_started_us = (int64_t)now_us;
        s_render_total_us = 0U;
        s_render_max_us = 0U;
    }
}


static void ui_unregister_service_callbacks(void)
{
    (void)object_service_set_event_callback(NULL, NULL);
    (void)storage_service_set_event_callback(NULL, NULL);
}


static void ui_delete_refresh_timer(void)
{
    if (s_context.refresh_timer == NULL) {
        return;
    }

    if (!lvgl_port_lock(500)) {
        ESP_LOGW(TAG, "Unable to lock LVGL while deleting refresh timer");
        return;
    }

    if (s_context.refresh_timer != NULL) {
        lv_timer_delete(s_context.refresh_timer);
        s_context.refresh_timer = NULL;
    }

    lvgl_port_unlock();
}


static void ui_runtime_init_rollback(void)
{
    ui_event_dispatcher_stop();
    ui_unregister_service_callbacks();
    atomic_store_explicit(
        &s_context.runtime_ready,
        false,
        memory_order_release
    );
    ui_delete_refresh_timer();
}


esp_err_t ui_boot_init(void)
{
    if (s_context.port_ready) {
        return ESP_OK;
    }

    memset(&s_context, 0, sizeof(s_context));
    atomic_init(
        &s_context.runtime_ready,
        false
    );

    esp_err_t err = ui_lvgl_init();
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

    ui_render_boot_locked("STARTING...");
    lvgl_port_unlock();

    ESP_LOGI(TAG, "LVGL boot UI ready");
    return ESP_OK;
}


void ui_boot_set_stage(const char *stage)
{
    if (!s_context.port_ready || stage == NULL) {
        return;
    }

    if (!lvgl_port_lock(250)) {
        ESP_LOGW(TAG, "Unable to lock LVGL for boot stage '%s'", stage);
        return;
    }

    ui_render_boot_locked(stage);
    lvgl_port_unlock();
}


void ui_boot_show_error(const char *message)
{
    if (!s_context.port_ready || message == NULL) {
        return;
    }

    if (!lvgl_port_lock(500)) {
        return;
    }

    ui_render_boot_error_locked(message);
    lvgl_port_unlock();
}


void ui_boot_show_storage_recovery(
    const char *reason,
    const char *technical_code,
    const char *warning,
    const char *primary_hint,
    const char *retry_hint,
    const char *cancel_hint
)
{
    if (!s_context.port_ready) {
        return;
    }

    if (!lvgl_port_lock(500)) {
        return;
    }

    ui_render_boot_recovery_locked(
        reason,
        technical_code,
        warning,
        primary_hint,
        retry_hint,
        cancel_hint
    );
    lvgl_port_unlock();
}


void ui_boot_show_format_progress(unsigned percent)
{
    if (!s_context.port_ready) {
        return;
    }

    if (!lvgl_port_lock(500)) {
        return;
    }

    ui_render_boot_format_progress_locked(percent);
    lvgl_port_unlock();
}


void ui_boot_show_recovery_result(
    bool success,
    bool formatted,
    const char *error_name
)
{
    if (!s_context.port_ready) {
        return;
    }

    if (!lvgl_port_lock(500)) {
        return;
    }

    ui_render_boot_recovery_result_locked(success, formatted, error_name);
    lvgl_port_unlock();
}


esp_err_t ui_init(void)
{
    if (!s_context.port_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (
        atomic_load_explicit(
            &s_context.runtime_ready,
            memory_order_acquire
        )
    ) {
        return ESP_OK;
    }

    esp_err_t err = object_service_set_event_callback(
        ui_object_service_event,
        NULL
    );
    if (err != ESP_OK) {
        return err;
    }

    err = storage_service_set_event_callback(
        ui_storage_service_event,
        NULL
    );
    if (err != ESP_OK) {
        (void)object_service_set_event_callback(NULL, NULL);
        return err;
    }

    s_context.screen = UI_SCREEN_HOME;
    s_context.selected = 0;
    s_context.notice[0] = '\0';
    ui_i18n_set_locale(ui_i18n_nvs_load_locale());
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    ui_frame_scheduler_init(&s_frame_scheduler, now_us);
    s_refresh_generation = 0U;
    s_perf_started_us = (int64_t)now_us;
    s_render_total_us = 0U;
    s_render_max_us = 0U;

    if (!lvgl_port_lock(1000)) {
        ui_runtime_init_rollback();
        return ESP_ERR_TIMEOUT;
    }

    ui_render_current_locked();

    s_context.refresh_timer = lv_timer_create(
        ui_refresh_timer_callback,
        42,
        NULL
    );

    lvgl_port_unlock();

    if (s_context.refresh_timer == NULL) {
        ui_runtime_init_rollback();
        return ESP_ERR_NO_MEM;
    }

    err = ui_event_dispatcher_start();
    if (err != ESP_OK) {
        ui_runtime_init_rollback();
        return err;
    }

    /*
     * This release store is the interactive-runtime publication boundary.
     * Service callbacks, the LVGL timer and keypad ingress may only mutate UI
     * after all callbacks, timer state and APP_EVENT ingress are installed.
     */
    atomic_store_explicit(
        &s_context.runtime_ready,
        true,
        memory_order_release
    );

    ESP_LOGI(TAG, "Interactive LVGL UI ready");
    return ESP_OK;
}


void ui_shutdown(void)
{
    if (!s_context.port_ready) {
        return;
    }

    /*
     * Stop new APP_EVENT ingress first. Then close service callback slots and
     * wait for callbacks that already snapped them. Only after those barriers
     * complete is it safe to publish runtime_ready=false and tear LVGL down.
     */
    ui_event_dispatcher_stop();
    ui_unregister_service_callbacks();

    atomic_store_explicit(
        &s_context.runtime_ready,
        false,
        memory_order_release
    );

    ui_delete_refresh_timer();

    if (lvgl_port_lock(500)) {
        if (s_context.root != NULL) {
            lv_obj_clean(s_context.root);
            lv_obj_set_style_bg_color(
                s_context.root,
                lv_color_hex(0x000000),
                0
            );
        }
        lvgl_port_unlock();
    }

    ui_lvgl_deinit();

    ESP_LOGI(TAG, "UI stopped");
}


void ui_handle_keypad_event(
    keypad_key_t key,
    void *user_context
)
{
    (void)user_context;

    if (
        !atomic_load_explicit(
            &s_context.runtime_ready,
            memory_order_acquire
        )
    ) {
        return;
    }

    esp_err_t err = app_events_post(
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
