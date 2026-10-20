#include "ui_internal.h"

#include <string.h>

#if !defined(_MSC_VER)
#include "esp_attr.h"
#endif
#include "esp_err.h"
#include "esp_log.h"
#include "ui_measurements_route.h"
#include "workflow_service.h"


static const char *TAG = "UI_SCREEN";

/* Navigation is serialized by the LVGL task lock; keep this route snapshot in PSRAM. */
#if defined(_MSC_VER)
static workflow_snapshot_t s_measurements_route_snapshot;
#else
static EXT_RAM_BSS_ATTR workflow_snapshot_t s_measurements_route_snapshot;
#endif

#if defined(_MSC_VER)
/* The standalone MSVC navigation fixture intentionally omits these full UI screens. */
void ui_workflow_screen_open_locked(void) {}
void ui_workflow_screen_close_locked(void) {}
void ui_workflow_screen_render_locked(void) {}
void ui_workflow_screen_refresh_locked(void) {}
void ui_workflow_screen_handle_key_locked(keypad_key_t key) { (void)key; }
void ui_scan_preview_refresh_locked(void) {}
void ui_home_refresh_locked(void) {}
void ui_render_hardware_locked(void) {}
#endif


static void ui_screen_open_home(void)
{
    ui_context_t *context = ui_context();

    context->selected = 0;
    context->storage_erase_confirm = false;
    context->wifi_confirm = UI_WIFI_CONFIRM_NONE;
    ui_set_notice_locked(NULL);
    ui_render_home_locked();
}


static void ui_screen_open_objects(void)
{
    ui_context_t *context = ui_context();

    context->selected = 0;
    context->objects_mode = context->objects_workflow_picker_on_open
        ? UI_OBJECTS_SELECT_WORKFLOW
        : UI_OBJECTS_MENU;
    context->objects_workflow_picker_on_open = false;
    context->object_action = 0;
    context->object_index = 0;
    context->object_count = 0;
    context->objects_loaded = false;

    if (context->storage_recovery_active) {
        ui_set_notice_locked(ui_tr(UI_I18N_KEY_SHARED_CHECKING_STORAGE));
        ui_render_objects_locked();
        return;
    }

    esp_err_t err = object_service_list_async();

    ui_set_notice_locked(
        err == ESP_OK
            ? ui_tr(UI_I18N_KEY_OBJECTS_LOADING)
            : ui_tr(UI_I18N_KEY_OBJECTS_LIST_FAILED)
    );

    ui_render_objects_locked();
}


static void ui_screen_open_measurements(void)
{
    ui_context_t *context = ui_context();

    context->selected = 0;
    context->measurement_action = 0;
    ui_set_notice_locked(NULL);

    /*
     * The AD7606 does not sample at boot; entering the Measurements screen is
     * the local trigger that brings acquisition up so the ADC only runs
     * while someone is actually looking at live channel data. Idempotent:
     * a Web client may have already started it via acquisition_start.
     */
    const esp_err_t acquisition_err = seismic_acquisition_start();
    if (acquisition_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Seismic acquisition unavailable on Measurements entry: %s",
            esp_err_to_name(acquisition_err)
        );
    }

    ui_render_measurements_locked();
}


static void ui_screen_open_storage(void)
{
    ui_context_t *context = ui_context();

    context->selected = 0;
    context->storage_action = 0;
    context->storage_erase_confirm = false;
    ui_set_notice_locked(NULL);

    (void)storage_service_info_async();
    ui_render_storage_locked();
}


static void ui_screen_open_wifi(void)
{
    ui_context_t *context = ui_context();

    context->selected = 0;
    context->wifi_action = 0;
    context->wifi_confirm = UI_WIFI_CONFIRM_NONE;
    ui_set_notice_locked(NULL);
    ui_render_wifi_locked();
}


static void ui_screen_open_system(void)
{
    ui_context()->system_hardware_open = false;
    ui_context()->system_hardware_selected = false;
    ui_context()->system_language_selection = ui_i18n_get_locale();
    ui_set_notice_locked(NULL);
    ui_render_system_locked();
}


static void ui_screen_open_scan_preview(void)
{
    ui_scan_preview_screen_open_locked();
}


static void ui_screen_handle_home_key(
    keypad_key_t key
)
{
    ui_context_t *context = ui_context();
    const size_t count = ui_screen_home_count();

    if (key == KEYPAD_KEY_UP) {
        ui_move_index(&context->selected, count, false);
        ui_render_home_locked();
        return;
    }

    if (key == KEYPAD_KEY_DOWN) {
        ui_move_index(&context->selected, count, true);
        ui_render_home_locked();
        return;
    }

    if (
        key == KEYPAD_KEY_OK ||
        key == KEYPAD_KEY_RIGHT
    ) {
        ui_screen_open_locked(
            ui_screen_home_at(context->selected)
        );
    }
}


static const ui_screen_driver_t s_screens[UI_SCREEN_COUNT] = {
    [UI_SCREEN_HOME] = {
        .name = "HOME",
        .home_label = UI_I18N_KEY_COUNT,
        .open = ui_screen_open_home,
        .render = ui_render_home_locked,
        .handle_key = ui_screen_handle_home_key,
        .refresh = ui_home_refresh_locked,
    },

    [UI_SCREEN_OBJECTS] = {
        .name = "OBJECTS",
        .home_label = UI_I18N_KEY_MENU_OBJECTS,
        .open = ui_screen_open_objects,
        .render = ui_render_objects_locked,
        .handle_key = ui_handle_objects_key_locked,
        .refresh = NULL,
    },

    [UI_SCREEN_MEASUREMENTS] = {
        .name = "MEASUREMENTS",
        .home_label = UI_I18N_KEY_MENU_MEASUREMENTS,
        .open = ui_screen_open_measurements,
        .render = ui_render_measurements_locked,
        .handle_key = ui_handle_measurements_key_locked,
        .refresh = ui_measurements_refresh_locked,
    },

    [UI_SCREEN_STORAGE] = {
        .name = "STORAGE",
        .home_label = UI_I18N_KEY_MENU_STORAGE,
        .open = ui_screen_open_storage,
        .render = ui_render_storage_locked,
        .handle_key = ui_handle_storage_key_locked,
        .refresh = NULL,
    },

    [UI_SCREEN_WIFI] = {
        .name = "WI-FI / WEB",
        .home_label = UI_I18N_KEY_WIFI_TITLE,
        .open = ui_screen_open_wifi,
        .render = ui_render_wifi_locked,
        .handle_key = ui_handle_wifi_key_locked,
        .refresh = NULL,
    },

    [UI_SCREEN_SYSTEM] = {
        .name = "SYSTEM",
        .home_label = UI_I18N_KEY_SYSTEM_TITLE,
        .open = ui_screen_open_system,
        .render = ui_render_system_locked,
        .handle_key = ui_system_handle_key_locked,
        .refresh = ui_system_refresh_locked,
    },

    [UI_SCREEN_SCAN_PREVIEW] = {
        .name = "3D PREVIEW",
        .home_label = UI_I18N_KEY_COUNT,
        .open = ui_screen_open_scan_preview,
        .render = ui_scan_preview_render_locked,
        .handle_key = ui_scan_preview_handle_key_locked,
        .refresh = ui_scan_preview_refresh_locked,
    },

    [UI_SCREEN_WORKFLOW] = {
        .name = "WORKFLOW",
        .home_label = UI_I18N_KEY_COUNT,
        .open = ui_workflow_screen_open_locked,
        .render = ui_workflow_screen_render_locked,
        .handle_key = ui_workflow_screen_handle_key_locked,
        .refresh = ui_workflow_screen_refresh_locked,
    },
};


/*
 * Home-visible screens are explicit rather than inferred from enum order.
 * Future modal/diagnostic screens can therefore be added to UI_SCREEN_* without
 * silently appearing in the Home menu.
 */
static const ui_screen_t s_home_screens[] = {
    UI_SCREEN_OBJECTS,
    UI_SCREEN_MEASUREMENTS,
    UI_SCREEN_STORAGE,
    UI_SCREEN_WIFI,
    UI_SCREEN_SYSTEM,
};


const ui_screen_driver_t *ui_screen_driver(
    ui_screen_t screen
)
{
    if (
        screen < UI_SCREEN_HOME ||
        screen >= UI_SCREEN_COUNT
    ) {
        return NULL;
    }

    return &s_screens[screen];
}


size_t ui_screen_home_count(void)
{
    return sizeof(s_home_screens) / sizeof(s_home_screens[0]);
}


ui_screen_t ui_screen_home_at(
    size_t index
)
{
    if (index >= ui_screen_home_count()) {
        return UI_SCREEN_HOME;
    }

    return s_home_screens[index];
}


const char *ui_screen_home_label(
    size_t index
)
{
    const ui_screen_driver_t *driver =
        ui_screen_driver(ui_screen_home_at(index));

    if (
        driver == NULL ||
        driver->home_label >= UI_I18N_KEY_COUNT
    ) {
        return ui_tr(UI_I18N_KEY_SHARED_EMPTY);
    }

    return ui_tr(driver->home_label);
}


ui_measurements_route_t ui_measurements_route_decide(
    workflow_step_t workflow_step,
    bool has_active_object)
{
    if (workflow_step == WORKFLOW_STEP_RECORD_READY ||
        workflow_step == WORKFLOW_STEP_RECORDING) {
        return UI_MEASUREMENTS_ROUTE_ALLOW;
    }
    return has_active_object
        ? UI_MEASUREMENTS_ROUTE_WORKFLOW
        : UI_MEASUREMENTS_ROUTE_OBJECT_PICKER;
}


void ui_screen_open_locked(
    ui_screen_t screen
)
{
    if (screen == UI_SCREEN_MEASUREMENTS) {
        memset(&s_measurements_route_snapshot, 0, sizeof(s_measurements_route_snapshot));
        const bool workflow_available =
            workflow_service_try_get_snapshot(&s_measurements_route_snapshot) == ESP_OK;
        bool has_active_object = workflow_available &&
            s_measurements_route_snapshot.object_id[0] != '\0';
        if (!has_active_object) {
            object_service_summary_t active_object;
            has_active_object = object_service_get_active(&active_object) == ESP_OK &&
                active_object.active && active_object.id[0] != '\0';
        }

        const ui_measurements_route_t route = ui_measurements_route_decide(
            workflow_available ? s_measurements_route_snapshot.step : WORKFLOW_STEP_IDLE,
            has_active_object);
        if (route == UI_MEASUREMENTS_ROUTE_WORKFLOW) {
            screen = UI_SCREEN_WORKFLOW;
        } else if (route == UI_MEASUREMENTS_ROUTE_OBJECT_PICKER) {
            ui_context()->objects_workflow_picker_on_open = true;
            screen = UI_SCREEN_OBJECTS;
        }
    }

    const ui_screen_driver_t *driver = ui_screen_driver(screen);

    if (
        driver == NULL ||
        driver->open == NULL
    ) {
        return;
    }

    const bool leaving_preview = ui_context()->screen == UI_SCREEN_SCAN_PREVIEW && screen != UI_SCREEN_SCAN_PREVIEW;
    const bool leaving_workflow = ui_context()->screen == UI_SCREEN_WORKFLOW &&
        screen != UI_SCREEN_WORKFLOW && screen != UI_SCREEN_SCAN_PREVIEW;
    if (leaving_preview) ui_scan_preview_close_locked();
    if (leaving_workflow) ui_workflow_screen_close_locked();
    ui_context()->screen = screen;
    driver->open();
    if (leaving_preview) ui_scan_preview_release_locked();
}


void ui_screen_render_locked(
    ui_screen_t screen
)
{
    const ui_screen_driver_t *driver = ui_screen_driver(screen);

    if (
        driver != NULL &&
        driver->render != NULL
    ) {
        driver->render();
    }
}


void ui_screen_handle_key_locked(
    ui_screen_t screen,
    keypad_key_t key
)
{
    const ui_screen_driver_t *driver = ui_screen_driver(screen);

    if (
        driver != NULL &&
        driver->handle_key != NULL
    ) {
        driver->handle_key(key);
    }
}


void ui_screen_refresh_locked(
    ui_screen_t screen
)
{
    const ui_screen_driver_t *driver = ui_screen_driver(screen);

    if (
        driver != NULL &&
        driver->refresh != NULL
    ) {
        driver->refresh();
    }
}
