#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_internal.h"
#include "ui_perf_policy.h"
#include "ui_fonts.h"
#include "hw_inventory.h"
#include "device_status.h"
#include "app_events.h"

/* The real screen owns its canvas, status labels and view state. */
#ifdef _MSC_VER
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#pragma warning(push)
#pragma warning(disable: 4701)
#endif
#include "../../components/ui/ui_scan_preview.c"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

extern ui_context_t render_context;
extern workflow_snapshot_t render_snapshot;
extern device_status_snapshot_t render_device_status;
extern measurement_service_snapshot_t render_measurement;
extern hw_module_info_t render_modules[HW_MODULE_COUNT];
extern bool render_wifi_setup, render_wifi_client, render_wifi_saved, render_wifi_ip;
extern bool render_active_object, render_required_missing, render_scope_available, render_storage_ready, render_service_busy;
extern esp_err_t render_service_result, render_measurement_result;
extern unsigned render_acquisition_mode;
extern unsigned render_delete_calls;
void render_capture_menu(const char *family, const char *variant);

const char *render_menus_widget_role(lv_obj_t *object)
{
    if (object == s_canvas) return "preview_canvas";
    if (object == s_status_label) return "preview_status";
    if (object == s_info_label) return "preview_info";
    if (object == s_legend_label) return "preview_legend";
    return "";
}

int render_preview_status(void) { return (int)s_state.status; }

#ifdef UI_NATIVE_FULL_SCREENS
#include "ui_navigation_fixture.h"
bool render_preview_open(void) { return s_state_initialized && s_state.open; }
extern bool render_fake_storage_enabled;
void render_preview_load_for_test(bool *ready, bool *fallback, size_t *sensors, size_t *sources)
{
    preview_request_t request = {.generation = s_state.generation};
    (void)snprintf(request.object_id, sizeof(request.object_id), "O0");
    (void)snprintf(request.session_id, sizeof(request.session_id), "S0");
    preview_result_t result;
    memset(&result, 0, sizeof(result));
    render_fake_storage_enabled = true;
    load_session(&request, &result);
    render_fake_storage_enabled = false;
    *ready = result.status == SCAN_PREVIEW_SCREEN_READY;
    *fallback = result.sensor_estimates_fallback;
    *sensors = geometry_sensor_count(&result.geometry);
    *sources = result.source_count;
    release_result(&result);
}
void render_preview_complete(bool stale)
{
    const uint32_t generation = s_state.generation;
    preview_request_t request = {.generation = stale ? generation - 1U : generation};
    preview_result_t result = {.status = SCAN_PREVIEW_SCREEN_NO_PREVIEW};
    (void)snprintf(result.fallback_reason, sizeof(result.fallback_reason), "fixture completion");
    result.draw_order = heap_caps_malloc(sizeof(*result.draw_order), MALLOC_CAP_8BIT);
    assert(result.draw_order != NULL);
    accept_result_locked(&request, &result);
    release_result(&result);
    assert(s_state.generation == generation);
    if (!stale && s_state.open) assert(strcmp(s_state.fallback_reason, "fixture completion") == 0);
}
#endif

static void reset_menu(ui_screen_t screen)
{
    ui_scan_preview_close_locked();
    ui_scan_preview_release_locked();
    lv_obj_t *previous = render_context.root;
    lv_display_t *display = render_context.display;
    memset(&render_context, 0, sizeof(render_context));
    render_context.root = previous;
    render_context.display = display;
    render_context.screen = screen;
    render_wifi_setup = render_wifi_client = render_wifi_saved = render_wifi_ip = false;
    render_active_object = render_required_missing = render_storage_ready = render_service_busy = false;
    render_scope_available = true;
    render_service_result = render_measurement_result = ESP_OK;
    render_acquisition_mode = 0U;
    memset(&render_measurement, 0, sizeof(render_measurement));
    memset(&render_snapshot, 0, sizeof(render_snapshot));
    for (size_t i = 0U; i < HW_MODULE_COUNT; ++i) {
        render_modules[i] = (hw_module_info_t){.id = (hw_module_id_t)i,
            .state = HW_STATE_PRESENT, .requirement = i < 2U ? HW_REQ_REQUIRED : HW_REQ_OPTIONAL};
    }
}

static void objects(unsigned count)
{
    render_context.objects_loaded = true;
    render_context.object_count = count;
    render_active_object = count > 0U;
    for (unsigned i = 0U; i < count; ++i) {
        object_service_summary_t *object = &render_context.objects[i];
        (void)snprintf(object->id, sizeof(object->id), "O%04u", i);
        memset(object->name, 'W', sizeof(object->name) - 1U);
        object->name[sizeof(object->name) - 1U] = '\0';
        object->active = i == 0U;
    }
}

static void capture_objects(void)
{
    reset_menu(UI_SCREEN_OBJECTS);
    ui_render_objects_locked();
    render_capture_menu("objects", "loading");
    render_context.objects_loaded = true;
    ui_render_objects_locked();
    render_capture_menu("objects", "empty");
    objects(OBJECT_SERVICE_MAX_OBJECTS);
    for (size_t selected = 0U; selected < 4U; ++selected) {
        char name[40];
        render_context.object_action = selected;
        ui_render_objects_locked();
        (void)snprintf(name, sizeof(name), "main_a%u", (unsigned)selected);
        render_capture_menu("objects", name);
    }
    for (ui_objects_mode_t mode = UI_OBJECTS_SELECT_ACTIVE; mode <= UI_OBJECTS_SELECT_WORKFLOW; ++mode) {
        render_context.objects_mode = mode;
        const size_t indices[] = {0U, 3U, 4U, OBJECT_SERVICE_MAX_OBJECTS - 1U};
        for (size_t i = 0U; i < sizeof(indices) / sizeof(indices[0]); ++i) {
            char name[40];
            render_context.object_index = indices[i];
            ui_render_objects_locked();
            (void)snprintf(name, sizeof(name), "list_m%u_i%u", (unsigned)mode, (unsigned)indices[i]);
            render_capture_menu("objects", name);
        }
        render_context.object_count = 0U;
        ui_render_objects_locked();
        char name[40];
        (void)snprintf(name, sizeof(name), "list_m%u_empty", (unsigned)mode);
        render_capture_menu("objects", name);
        render_context.object_count = OBJECT_SERVICE_MAX_OBJECTS;
    }
    render_context.objects_mode = UI_OBJECTS_DELETE_CONFIRM;
    object_store_delete_scope_t scope = {.sessions = UINT32_MAX, .artifacts = UINT32_MAX, .localization_runs = UINT32_MAX};
    for (unsigned available = 0U; available < 2U; ++available) {
        ui_objects_delete_model_build(available != 0U, &scope, &render_context.object_delete_model);
        ui_render_objects_locked();
        render_capture_menu("objects", available != 0U ? "delete_scope" : "delete_scope_unavailable");
    }
    render_context.objects_mode = UI_OBJECTS_MENU;
    const ui_i18n_key_t notices[] = {UI_I18N_KEY_OBJECTS_CREATING, UI_I18N_KEY_OBJECTS_SERVICE_BUSY,
        UI_I18N_KEY_OBJECTS_SET_ACTIVE_START_FAILED, UI_I18N_KEY_OBJECTS_DELETE_START_FAILED,
        UI_I18N_KEY_SHARED_CHECKING_STORAGE, UI_I18N_KEY_SHARED_MEASURE_SERVICE_UNAVAILABLE};
    for (size_t i = 0U; i < sizeof(notices) / sizeof(notices[0]); ++i) {
        char name[40];
        ui_set_notice_locked(ui_tr(notices[i]));
        ui_render_objects_locked();
        (void)snprintf(name, sizeof(name), "notice_%u", (unsigned)i);
        render_capture_menu("objects", name);
    }
    reset_menu(UI_SCREEN_OBJECTS);
    objects(1U);
    render_delete_calls = 0U;
    render_context.object_action = 2U;
    ui_render_objects_locked();
    ui_handle_objects_key_locked(KEYPAD_KEY_OK);
    ui_storage_query_gate_open();
    ui_handle_objects_key_locked(KEYPAD_KEY_OK);
    render_capture_menu("objects", "key_delete_confirmation");
    ui_handle_objects_key_locked(KEYPAD_KEY_LEFT);
    render_capture_menu("objects", "key_delete_cancel");
}

static void capture_storage(void)
{
    for (unsigned ready = 0U; ready < 2U; ++ready) {
        reset_menu(UI_SCREEN_STORAGE);
        render_context.storage_info_valid = ready != 0U;
        render_storage_ready = ready != 0U;
        (void)snprintf(render_context.storage_info.name, sizeof(render_context.storage_info.name), "SDTEST");
        (void)snprintf(render_context.storage_info.type, sizeof(render_context.storage_info.type), "SDHC/SDXC-FIXTURE");
        render_context.storage_info.total_bytes = UINT64_C(2048) * 1024U * 1024U * 1024U;
        render_context.storage_info.free_bytes = render_context.storage_info.total_bytes - 1024U;
        render_context.storage_info.speed_khz = 40000U;
        for (unsigned selected = 0U; selected < 2U; ++selected) {
            char name[40];
            render_context.storage_action = selected;
            ui_render_storage_locked();
            (void)snprintf(name, sizeof(name), "ready%u_a%u", ready, selected);
            render_capture_menu("storage", name);
        }
        if (ready != 0U) {
            render_context.storage_erase_confirm = true;
            ui_render_storage_locked();
            render_capture_menu("storage", "erase_confirm");
        }
        ui_set_notice_locked(ui_tr(UI_I18N_KEY_STORAGE_SERVICE_BUSY));
        ui_render_storage_locked();
        render_capture_menu("storage", ready != 0U ? "busy_ready" : "busy_unavailable");
    }
}

static void capture_wifi(void)
{
    /* Every setup branch, saved-network/verify state, confirmation and selection. */
    for (unsigned state = 0U; state < 15U; ++state) {
        reset_menu(UI_SCREEN_WIFI);
        render_wifi_saved = state != 0U;
        /* States 13 and 14 are the verify QR card (9) and the verified card (10) while the station has no address. */
        render_wifi_ip = state >= 8U && state < 13U;
        if (state >= 1U && state <= 7U) {
            render_wifi_setup = true;
            render_wifi_client = state >= 2U && state != 7U;
            render_context.wifi_ca_downloaded = state >= 3U;
            render_context.wifi_ca_install_confirmed = state >= 4U;
            render_context.wifi_setup_page_opened = state >= 5U;
            render_context.wifi_credentials_saved = state >= 6U;
        }
        if (state >= 9U) {
            render_context.wifi_verify_ready = true;
            render_context.wifi_verify_nonce = state == 12U ? 0U : UINT32_C(0x1234ABCD);
            render_context.wifi_verify_result_received = state == 10U || state == 11U || state == 14U;
            render_context.wifi_verify_result_mask =
                (state == 10U || state == 14U) ? APP_TLS_VERIFY_REQUIRED_MASK : APP_TLS_VERIFY_CAMERA;
        }
        for (unsigned selected = 0U; selected < 3U; ++selected) {
            char name[48];
            render_context.wifi_action = selected;
            ui_render_wifi_locked();
            (void)snprintf(name, sizeof(name), "state%02u_a%u", state, selected);
            render_capture_menu("wifi", name);
        }
    }
    for (ui_wifi_confirm_t confirm = UI_WIFI_CONFIRM_SETUP; confirm <= UI_WIFI_CONFIRM_FORGET; ++confirm) {
        reset_menu(UI_SCREEN_WIFI);
        render_wifi_saved = true;
        render_context.wifi_action = confirm == UI_WIFI_CONFIRM_SETUP ? 0U : 2U;
        render_context.wifi_confirm = confirm;
        ui_set_notice_locked(ui_tr(confirm == UI_WIFI_CONFIRM_SETUP ? UI_I18N_KEY_WIFI_ASK_SETUP : UI_I18N_KEY_WIFI_ASK_FORGET));
        ui_render_wifi_locked();
        render_capture_menu("wifi", confirm == UI_WIFI_CONFIRM_SETUP ? "confirm_setup" : "confirm_forget");
    }
    ui_set_notice_locked(ui_tr(UI_I18N_KEY_WIFI_COMMAND_FAILED));
    ui_render_wifi_locked();
    render_capture_menu("wifi", "command_error");
}

static void capture_system(void)
{
    for (unsigned selected = 0U; selected < 3U; ++selected) {
        reset_menu(UI_SCREEN_SYSTEM);
        render_context.system_language_selection = selected == 1U ? UI_LOCALE_UK : UI_LOCALE_EN;
        render_context.system_hardware_selected = selected == 2U;
        ui_render_system_locked();
        char name[40];
        (void)snprintf(name, sizeof(name), "selection%u", selected);
        render_capture_menu("system", name);
    }
    for (unsigned state = 0U; state < 5U; ++state) {
        reset_menu(UI_SCREEN_SYSTEM);
        render_context.system_hardware_open = true;
        render_snapshot.assigned_remotes = (uint8_t)state;
        for (size_t i = 0U; i < HW_MODULE_COUNT; ++i) render_modules[i].state = (hw_module_state_t)state;
        for (size_t i = 0U; i < DEVICE_STATUS_POD_COUNT; ++i) render_device_status.pods[i].state = (pod_state_t)(i % 4U);
        ui_render_system_locked();
        char name[40];
        (void)snprintf(name, sizeof(name), "module_state%u", state);
        render_capture_menu("hardware", name);
    }
}

extern bool render_synth_waveform;
extern uint64_t render_now_ms;

static unsigned char s_dirty_rows[240];
static int s_dirty_widest, s_dirty_tallest;

static void collect_dirty_rows(lv_event_t *event)
{
    const lv_area_t *area = lv_event_get_param(event);
    if (area == NULL) return;
    const int width = (int)(area->x2 - area->x1 + 1), height = (int)(area->y2 - area->y1 + 1);
    if (width > s_dirty_widest) s_dirty_widest = width;
    if (height > s_dirty_tallest) s_dirty_tallest = height;
    for (int32_t y = area->y1 < 0 ? 0 : area->y1; y <= area->y2 && y < 240; ++y) s_dirty_rows[y] = 1U;
}

/* Rows and flush chunks (UI_PERF_FLUSH_CHUNK_ROWS rows each) that one refresh marks for redraw. */
static void measure_live_refresh(unsigned *rows, unsigned *chunks)
{
    lv_display_t *display = lv_display_get_default();
    lv_event_dsc_t *listener = lv_display_add_event_cb(display, collect_dirty_rows, LV_EVENT_INVALIDATE_AREA, NULL);
    assert(listener != NULL);
    memset(s_dirty_rows, 0, sizeof(s_dirty_rows));
    s_dirty_widest = s_dirty_tallest = 0;
    ui_measurements_refresh_locked();
    *rows = 0U;
    *chunks = 0U;
    for (unsigned y = 0U; y < 240U; ++y) *rows += s_dirty_rows[y];
    for (unsigned chunk = 0U; chunk < 240U / UI_PERF_FLUSH_CHUNK_ROWS; ++chunk) {
        for (unsigned y = chunk * UI_PERF_FLUSH_CHUNK_ROWS; y < (chunk + 1U) * UI_PERF_FLUSH_CHUNK_ROWS; ++y) {
            if (s_dirty_rows[y] != 0U) { ++*chunks; break; }
        }
    }
    assert(lv_display_remove_event_cb_with_user_data(display, collect_dirty_rows, NULL) == 1U);
    lv_refr_now(display);
}

/*
 * Bench log 2026-11-01 21:46: a frame of the live charts cost ~340 ms because one update marked the whole block
 * of the four cards (300x86, 12 of the 30 eight-row flush chunks) for redraw; a chunk costs ~20 ms of core 0 on
 * this hardware, with the acquisition off as well (the one large-area frame of log 13:12 cost the same 20 ms per
 * chunk). An update of the charts must dirty the four 58x14 chart strips only, and the channel value lines
 * follow once a second.
 */
static void test_live_chart_update_dirties_only_what_changed(void)
{
    const uint64_t clock_before_ms = render_now_ms;   /* later captures show the uptime */
    reset_menu(UI_SCREEN_MEASUREMENTS);
    render_measurement.state = MEASUREMENT_SERVICE_LIVE;
    render_measurement.active_object_present = true;
    render_synth_waveform = true;
    ui_render_measurements_locked();                  /* creates the screen and does the first refresh */
    lv_refr_now(lv_display_get_default());
    unsigned rows = 0U, chunks = 0U;

    /* 200 ms later: new samples, the charts move, the values are not yet due. */
    render_now_ms += 200U;
    measure_live_refresh(&rows, &chunks);
    assert(rows > 0U);
    assert(s_dirty_widest <= 58 && s_dirty_tallest <= 14);      /* the strips, never the block of the cards */
    assert(rows == 28U && chunks <= 5U);

    /* A second after the values were written they are redrawn with the charts: two bands of label lines more. */
    render_now_ms += 1000U;
    measure_live_refresh(&rows, &chunks);
    assert(rows > 28U && chunks <= 10U);
    assert(s_dirty_widest <= 146 && s_dirty_tallest <= 24);

    /* Faster than the chart interval nothing is touched at all. */
    render_now_ms += 20U;
    measure_live_refresh(&rows, &chunks);
    assert(rows == 0U && chunks == 0U);
    render_synth_waveform = false;
    render_now_ms = clock_before_ms;
}

static int text_width_px(const char *text)
{
    lv_point_t size;
    lv_text_get_size(&size, text, &ui_font_montserrat_14, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return (int)size.x;
}

/*
 * Bench log 2026-11-02 10:36: the live screen had 6 (EN) to 11 (UK) labels wider than their boxes. A label that
 * does not fit scrolls for ever (LV_LABEL_LONG_SCROLL_CIRCULAR) and every step redraws its strip, so LVGL drew 11
 * flush chunks per pass at ~14 ms each with no new sample. The texts the screen shows are written to fit.
 */
static void test_the_strings_of_the_live_screen_fit_their_boxes(void)
{
    const ui_locale_t locale_before = ui_i18n_get_locale();
    const ui_locale_t locales[2] = {UI_LOCALE_EN, UI_LOCALE_UK};
    for (unsigned locale = 0U; locale < 2U; ++locale) {
        ui_i18n_set_locale(locales[locale]);
        assert(text_width_px(ui_tr(UI_I18N_KEY_MEASUREMENT_TITLE)) <= UI_HEADER_TITLE_WIDTH);
        assert(text_width_px(ui_tr(UI_I18N_KEY_MEASUREMENT_FOOTER)) <= UI_FOOTER_LABEL_WIDTH);
        assert(text_width_px(ui_tr(UI_I18N_KEY_MEASUREMENT_LINK_LEGEND)) <= UI_MEASUREMENT_LEGEND_LABEL_WIDTH);
        const ui_i18n_key_t links[] = {
            UI_I18N_KEY_MEASUREMENT_LINK_NOT_CONNECTED, UI_I18N_KEY_MEASUREMENT_LINK_CONNECTED,
            UI_I18N_KEY_MEASUREMENT_LINK_READY, UI_I18N_KEY_MEASUREMENT_LINK_MEASURING,
        };
        for (size_t i = 0U; i < sizeof(links) / sizeof(links[0]); ++i) {
            assert(text_width_px(ui_tr(links[i])) <= UI_MEASUREMENT_LINK_LABEL_WIDTH);
        }
        const double values_mv[] = {-99.99, -7.2, 0.04, 12.34, 99.99};
        const ui_i18n_key_t kinds[] = {UI_I18N_KEY_MEASUREMENT_CHANNEL_FLAT, UI_I18N_KEY_MEASUREMENT_CHANNEL_VARIABLE};
        for (size_t v = 0U; v < sizeof(values_mv) / sizeof(values_mv[0]); ++v) {
            for (size_t k = 0U; k < 2U; ++k) {
                char text[64];
                (void)ui_utf8_format(text, sizeof(text), ui_tr(UI_I18N_KEY_MEASUREMENT_CHANNEL_VALUE), 4U, values_mv[v], ui_tr(kinds[k]));
                assert(text_width_px(text) <= UI_MEASUREMENT_CHANNEL_LABEL_WIDTH);
            }
        }
    }
    ui_i18n_set_locale(locale_before);
}

static void test_the_live_screen_runs_no_animation_of_its_own(void)
{
    const uint64_t clock_before_ms = render_now_ms;           /* later captures show the uptime */
    const ui_locale_t locale_before = ui_i18n_get_locale();   /* the fixtures run once per locale */
    const ui_locale_t locales[2] = {UI_LOCALE_EN, UI_LOCALE_UK};
    for (unsigned locale = 0U; locale < 2U; ++locale) {
        ui_i18n_set_locale(locales[locale]);
        reset_menu(UI_SCREEN_MEASUREMENTS);
        render_measurement.state = MEASUREMENT_SERVICE_LIVE;
        render_measurement.active_object_present = true;
        render_synth_waveform = true;
        ui_render_measurements_locked();
        lv_refr_now(lv_display_get_default());
        assert(lv_anim_count_running() == 0U);
        for (uint32_t step = 0U; step < 4U; ++step) {
            render_now_ms += 1000U;
            ui_measurements_refresh_locked();     /* values arrive: CHn +x.xxmV FLAT/VAR */
            lv_refr_now(lv_display_get_default());
            assert(lv_anim_count_running() == 0U);
        }
        render_synth_waveform = false;
    }
    ui_i18n_set_locale(locale_before);
    render_now_ms = clock_before_ms;
}

static void capture_measurements(void)
{
    for (measurement_service_state_t state = MEASUREMENT_SERVICE_LIVE; state <= MEASUREMENT_SERVICE_ERROR; ++state) {
        for (unsigned selected = 0U; selected < (state == MEASUREMENT_SERVICE_REVIEW ? 3U : 1U); ++selected) {
            reset_menu(UI_SCREEN_MEASUREMENTS);
            render_measurement.state = state;
            render_measurement.active_object_present = true;
            (void)snprintf(render_measurement.object_id, sizeof(render_measurement.object_id), "O0012");
            (void)snprintf(render_measurement.object_name, sizeof(render_measurement.object_name), "Long measurement object name for UI bounds");
            (void)snprintf(render_measurement.session_id, sizeof(render_measurement.session_id), "S0012");
            render_measurement.samples_written = UINT64_C(1234567890123);
            render_measurement.dropped_frames = UINT64_C(987654321);
            render_measurement.last_error = ESP_ERR_TIMEOUT;
            render_context.measurement_action = selected;
            ui_render_measurements_locked();
            char name[48];
            (void)snprintf(name, sizeof(name), "state%u_a%u", (unsigned)state, selected);
            render_capture_menu("measurements", name);
            for (unsigned move = 0U; move < 4U; ++move) ui_handle_measurements_key_locked(KEYPAD_KEY_DOWN);
            (void)snprintf(name, sizeof(name), "state%u_a%u_bottom", (unsigned)state, selected);
            render_capture_menu("measurements", name);
            if (state == MEASUREMENT_SERVICE_LIVE) {
                for (unsigned move = 0U; move < 4U; ++move) ui_handle_measurements_key_locked(KEYPAD_KEY_UP);
                render_capture_menu("measurements", "returned_top");
            }
        }
    }
    test_live_chart_update_dirties_only_what_changed();
    test_the_strings_of_the_live_screen_fit_their_boxes();
    test_the_live_screen_runs_no_animation_of_its_own();
    for (unsigned fault = 0U; fault < 6U; ++fault) {
        reset_menu(UI_SCREEN_MEASUREMENTS);
        render_acquisition_mode = fault;
        render_measurement_result = fault == 5U ? ESP_ERR_INVALID_STATE : ESP_OK;
        ui_render_measurements_locked();
        char name[40];
        (void)snprintf(name, sizeof(name), "transport%u", fault);
        render_capture_menu("measurements", name);
        for (unsigned move = 0U; move < 4U; ++move) ui_handle_measurements_key_locked(KEYPAD_KEY_DOWN);
        (void)snprintf(name, sizeof(name), "transport%u_bottom", fault);
        render_capture_menu("measurements", name);
    }
}

static void capture_boot(void)
{
    const char *stages[] = {NULL, "STARTING...", "STORAGE...", "RECOVERY...", "ACTIVE OBJECT...", "OBJECT STORE...", "NETWORK + UI..."};
    for (size_t i = 0U; i < sizeof(stages) / sizeof(stages[0]); ++i) {
        reset_menu(UI_SCREEN_HOME);
        ui_render_boot_locked(stages[i]);
        char name[40]; (void)snprintf(name, sizeof(name), "stage%u", (unsigned)i);
        render_capture_menu("boot", name);
    }
    const char *errors[] = {"MOUNT FAILED", "WRITE TEST FAILED", "ESP_ERR_INVALID_STATE: storage unavailable"};
    for (size_t i = 0U; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        ui_render_boot_error_locked(errors[i]);
        char name[40]; (void)snprintf(name, sizeof(name), "error%u", (unsigned)i);
        render_capture_menu("boot", name);
    }
    for (unsigned split = 0U; split < 2U; ++split) {
        for (unsigned warning = 0U; warning < 2U; ++warning) {
            ui_render_boot_recovery_locked(ui_tr(UI_I18N_KEY_BOOT_REASON_FS_MOUNT),
                split != 0U ? "ESP_ERR_INVALID_STATE long technical recovery detail" : "ESP_ERR_TIMEOUT",
                warning != 0U ? ui_tr(UI_I18N_KEY_BOOT_SD_LIMIT) : "",
                ui_tr(UI_I18N_KEY_BOOT_RECOVERY_FORMAT_HINT), ui_tr(UI_I18N_KEY_BOOT_RECOVERY_RETRY_HINT),
                ui_tr(UI_I18N_KEY_BOOT_RECOVERY_CANCEL_HINT));
            char name[40]; (void)snprintf(name, sizeof(name), "recovery_s%u_w%u", split, warning);
            render_capture_menu("boot", name);
        }
    }
    for (unsigned progress = 0U; progress <= 100U; progress += 50U) {
        ui_render_boot_format_progress_locked(progress);
        char name[40]; (void)snprintf(name, sizeof(name), "format%u", progress);
        render_capture_menu("boot", name);
    }
    for (unsigned success = 0U; success < 2U; ++success) {
        for (unsigned formatted = 0U; formatted < 2U; ++formatted) {
            ui_render_boot_recovery_result_locked(success != 0U, formatted != 0U,
                success != 0U ? "ESP_OK" : "ESP_ERR_INVALID_STATE");
            char name[40]; (void)snprintf(name, sizeof(name), "result_s%u_f%u", success, formatted);
            render_capture_menu("boot", name);
        }
    }
}

static void preview_base(void)
{
    reset_menu(UI_SCREEN_SCAN_PREVIEW);
    s_from_workflow = false;
    (void)snprintf(s_target_object_id, sizeof(s_target_object_id), "O0012");
    (void)snprintf(s_target_session_id, sizeof(s_target_session_id), "S0012");
    ui_scan_preview_screen_open_locked();
}

static void capture_preview(void)
{
    const char *reasons[] = {"preview unavailable", "invalid session id", "recording active", "scan missing",
        "scan unreadable", "preview memory", "stale scan CRC", "invalid preview", "preview missing",
        "preview unreadable", "no preview or occupancy", "no valid preview or occupancy", "screen allocation failed",
        "canvas memory unavailable", "loader unavailable"};
    for (size_t i = 0U; i < sizeof(reasons) / sizeof(reasons[0]); ++i) {
        preview_base();
        s_state.status = i == 3U ? SCAN_PREVIEW_SCREEN_NO_3D_SCAN : SCAN_PREVIEW_SCREEN_NO_PREVIEW;
        copy_string(s_state.fallback_reason, sizeof(s_state.fallback_reason), reasons[i]);
        render_current_locked();
        char name[40]; (void)snprintf(name, sizeof(name), "unavailable%u", (unsigned)i);
        render_capture_menu("preview", name);
    }
    preview_base();
    s_state.status = SCAN_PREVIEW_SCREEN_LOADING;
    s_state.fallback_reason[0] = '\0';
    lv_label_set_text(s_info_label, "");
    render_current_locked();
    render_capture_menu("preview", "loading");
    preview_base();
    s_state.status = SCAN_PREVIEW_SCREEN_READY;
    s_sensor_estimates_fallback = true;
    for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        s_geometry.pods[i].computed = true;
        s_geometry.pods[i].x_mm = (int32_t)(i % 2U) * 3000;
        s_geometry.pods[i].y_mm = (int32_t)(i / 2U) * 3000;
        s_geometry.pods[i].z_mm = (int32_t)i * 300;
    }
    s_source_count = 1U;
    s_sources[0] = (ui_preview3d_source_t){.position = {1.0f, 1.5f, -0.3f}, .selected = true};
    for (ui_preview3d_preset_t preset = UI_PREVIEW3D_PRESET_TOP; preset <= UI_PREVIEW3D_PRESET_FREE; ++preset) {
        ui_preview3d_view_preset(&s_view, preset);
        render_current_locked();
        char name[40]; (void)snprintf(name, sizeof(name), "sensors_view%u", (unsigned)preset);
        render_capture_menu("preview", name);
    }
    s_from_workflow = true;
    s_workflow_recording = true;
    render_snapshot.step = WORKFLOW_STEP_RECORDING;
    render_snapshot.generation = 1U;
    render_snapshot.allowed_actions = WORKFLOW_ACTION(WORKFLOW_COMMAND_STOP_RECORDING);
    preview_set_footer(UI_I18N_KEY_TFT_WORKFLOW_PREVIEW_STOP_HINT);
    render_capture_menu("preview", "recording");
    ui_scan_preview_handle_key_locked(KEYPAD_KEY_OK);
    render_capture_menu("preview", "stop_confirm");
    ui_scan_preview_handle_key_locked(KEYPAD_KEY_OK);
    render_capture_menu("preview", "stop_pending");
    s_stop_pending = false;
    s_stop_armed = true;
    render_service_result = ESP_ERR_NO_MEM;
    ui_scan_preview_handle_key_locked(KEYPAD_KEY_OK);
    render_capture_menu("preview", "stop_busy");
    s_stop_armed = true;
    render_service_result = ESP_ERR_INVALID_STATE;
    ui_scan_preview_handle_key_locked(KEYPAD_KEY_OK);
    render_capture_menu("preview", "stop_error");
    for (unsigned registered = 0U; registered < 2U; ++registered) {
        preview_base();
        static uint32_t indices[] = {0U, 1U, 4U, 5U, 16U, 17U, 20U, 21U};
        static uint8_t faces[] = {63U, 63U, 63U, 63U, 63U, 63U, 63U, 63U};
        s_state.status = SCAN_PREVIEW_SCREEN_READY;
        s_state.fallback_reason[0] = '\0';
        s_state.has_preview = true;
        s_state.preview = (scan_preview_t){.origin = {-1.0f, -1.0f, -1.0f}, .voxel_size_m = 1.0f,
            .dimensions = {4U, 4U, 4U}, .voxel_count = 8U, .indices = indices, .face_masks = faces,
            .registration_usable = registered != 0U, .geometry_computed_at_ms = 1000U};
        for (size_t i = 0U; i < 16U; ++i) s_state.preview.sensor_to_scan[i] = i % 5U == 0U ? 1.0f : 0.0f;
        s_geometry_computed_at_ms = 1000U;
        s_draw_order = malloc(8U * sizeof(*s_draw_order));
        assert(s_draw_order != NULL);
        s_source_count = 1U;
        s_sources[0] = (ui_preview3d_source_t){.position = {0.3f, 0.4f, 0.2f}, .selected = true,
            .geometry_computed_at_ms = 1000U, .uncertainty_valid = true,
            .horizontal_std_x_m = 0.2f, .horizontal_std_y_m = 0.3f, .vertical_std_m = 0.2f};
        for (ui_preview3d_preset_t view = UI_PREVIEW3D_PRESET_TOP; view <= UI_PREVIEW3D_PRESET_ISO; ++view) {
            ui_preview3d_view_preset(&s_view, view);
            render_current_locked();
            char name[48]; (void)snprintf(name, sizeof(name), "voxels_registered%u_view%u", registered, (unsigned)view);
            render_capture_menu("preview", name);
        }
        ui_scan_preview_handle_key_locked(KEYPAD_KEY_RIGHT);
        render_capture_menu("preview", registered != 0U ? "voxels_rotated_registered" : "voxels_rotated_unregistered");
    }
    {
        /* A real scan: the 119 cells a phone sent to the ESP32, through the production parser (not only the 2x2x2 cube above). */
        static const uint8_t real_preview[] = {
#include "real_scan_preview_v1.inc"
        };
        preview_base();
        scan_preview_t real = {0};
        const scan_preview_error_t parsed = scan_preview_parse(real_preview, sizeof(real_preview), NULL, &real);
        assert(parsed == SCAN_PREVIEW_OK && real.voxel_count == 119U && real.voxel_size_m == 0.25f);
        s_state.status = SCAN_PREVIEW_SCREEN_READY;
        s_state.fallback_reason[0] = '\0';
        s_state.has_preview = true;
        s_state.preview = real;
        s_geometry_computed_at_ms = 0U;
        s_draw_order = malloc((size_t)real.voxel_count * sizeof(*s_draw_order));
        assert(s_draw_order != NULL);
        s_source_count = 0U;
        for (ui_preview3d_preset_t view = UI_PREVIEW3D_PRESET_TOP; view <= UI_PREVIEW3D_PRESET_ISO; ++view) {
            ui_preview3d_view_preset(&s_view, view);
            render_current_locked();
            char name[48]; (void)snprintf(name, sizeof(name), "voxels_real_scan_view%u", (unsigned)view);
            render_capture_menu("preview", name);
        }
    }
    preview_base();
    s_state.status = SCAN_PREVIEW_SCREEN_READY;
    s_state.has_occupancy = true;
    s_state.fallback_reason[0] = '\0';
    s_state.occupancy.voxel_size_m = 0.75;
    s_state.occupancy.occupied_count = 8U;
    s_state.occupancy.occupied_keys = malloc(8U * sizeof(int64_t));
    assert(s_state.occupancy.occupied_keys != NULL);
    for (int x = 0; x < 2; ++x) for (int y = 0; y < 2; ++y) for (int z = 0; z < 2; ++z) {
        /* Fixture cells use the production occupancy artifact's packed int16 indices. */
        s_state.occupancy.occupied_keys[x * 4 + y * 2 + z] =
            ((int64_t)(x + 32767) << 32) | ((int64_t)(y + 32767) << 16) | (int64_t)(z + 32767);
    }
    render_current_locked();
    render_capture_menu("preview", "occupancy");
    ui_scan_preview_close_locked();
    ui_scan_preview_release_locked();
}

void render_all_menus(void)
{
    for (unsigned selected = 0U; selected < UI_HOME_SCREEN_COUNT; ++selected) {
        reset_menu(UI_SCREEN_HOME);
        render_context.selected = selected;
        ui_render_home_locked();
        char name[40]; (void)snprintf(name, sizeof(name), "selection%u", selected);
        render_capture_menu("home", name);
    }
    reset_menu(UI_SCREEN_HOME);
    render_required_missing = true;
    ui_render_home_locked(); render_capture_menu("home", "required_missing");
    render_required_missing = false;
    render_context.storage_recovery_active = true;
    ui_render_home_locked(); render_capture_menu("home", "storage_recovering");
    render_context.storage_recovery_active = false;
    render_context.storage_recovery_failed = true;
    ui_render_home_locked(); render_capture_menu("home", "storage_failed");
    capture_objects();
    capture_storage();
    capture_wifi();
    capture_system();
    capture_measurements();
    capture_boot();
    capture_preview();
}
