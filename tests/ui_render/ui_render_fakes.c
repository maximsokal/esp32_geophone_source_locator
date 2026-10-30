/* Hardware/service boundary only. Widgets, font, model and renderers are real. */
#include "ui_internal.h"
#include "ui_workflow_model.h"
#include "workflow_service.h"
#include "device_status.h"
#include "wifi_manager.h"
#include "hw_inventory.h"
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include "freertos/task.h"
#include "nrf_topology_service.h"
#include "localization_run_store.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "display.h"
#include "storage_manager.h"
#include "storage_bulk_admission.h"
#ifdef UI_NATIVE_FULL_SCREENS
#include <setjmp.h>
#include "ui_navigation_fixture.h"

static struct {
    void (*run)(void *);
    void *argument;
    const char *name;
} render_tasks[2];
static size_t render_task_count, render_selected_task;
static jmp_buf render_task_return;
static bool render_task_running, render_task_notified;
static void (*render_completion_interleave)(void);
unsigned render_heap_allocations;
unsigned render_heap_failures_remaining;
size_t render_heap_bytes;
static struct { void *memory; size_t size; } render_allocations[64];

void *render_heap_malloc(size_t size)
{
    if (render_heap_failures_remaining > 0U) {
        --render_heap_failures_remaining;
        return NULL;
    }
    void *memory = malloc(size);
    assert(memory != NULL);
    for (size_t i = 0U; i < 64U; ++i) {
        if (render_allocations[i].memory != NULL) continue;
        render_allocations[i].memory = memory;
        render_allocations[i].size = size;
        ++render_heap_allocations;
        render_heap_bytes += size;
        return memory;
    }
    assert(false);
    return NULL;
}
void *render_heap_calloc(size_t count, size_t size)
{
    assert(size == 0U || count <= SIZE_MAX / size);
    void *memory = render_heap_malloc(count * size);
    if (memory != NULL) memset(memory, 0, count * size);
    return memory;
}
void render_heap_free(void *memory)
{
    if (memory == NULL) return;
    for (size_t i = 0U; i < 64U; ++i) {
        if (render_allocations[i].memory != memory) continue;
        assert(render_heap_allocations > 0U && render_heap_bytes >= render_allocations[i].size);
        --render_heap_allocations;
        render_heap_bytes -= render_allocations[i].size;
        render_allocations[i].memory = NULL;
        free(memory);
        return;
    }
    assert(false);
}
void render_run_task(const char *name, void (*before_completion)(void))
{
    render_selected_task = render_task_count;
    for (size_t i = 0U; i < render_task_count; ++i) {
        if (strcmp(render_tasks[i].name, name) != 0) continue;
        render_selected_task = i;
        break;
    }
    assert(render_selected_task < render_task_count && !render_task_running);
    render_task_running = true;
    render_task_notified = true;
    render_completion_interleave = before_completion;
    /* Stop only at the worker's next notification wait, after its frees. */
    if (setjmp(render_task_return) == 0) render_tasks[render_selected_task].run(render_tasks[render_selected_task].argument);
    render_task_running = false;
    render_completion_interleave = NULL;
}
#endif

ui_context_t render_context;
workflow_snapshot_t render_snapshot;
device_status_snapshot_t render_device_status;
nrf_node_link_status_input_t render_node_status[UI_WORKFLOW_REMOTE_SLOT_COUNT + 1U];
uint64_t render_now_ms = 1000U;
workflow_command_t render_last_command = WORKFLOW_COMMAND_COUNT;
unsigned render_command_count;
unsigned render_delete_calls;
unsigned render_list_calls, render_active_calls, render_erase_calls, render_info_calls;
unsigned render_wifi_setup_calls, render_wifi_forget_calls, render_acquisition_starts;
esp_err_t render_acquisition_result = ESP_OK;
bool render_wifi_setup;
bool render_wifi_client;
bool render_wifi_saved;
bool render_wifi_ip;
bool render_active_object;
bool render_required_missing;
bool render_scope_available = true;
bool render_storage_ready;
bool render_service_busy;
esp_err_t render_service_result = ESP_OK;
esp_err_t render_workflow_snapshot_result = ESP_OK;
esp_err_t render_measurement_result = ESP_OK;
unsigned render_acquisition_mode;
measurement_service_snapshot_t render_measurement;
hw_module_info_t render_modules[HW_MODULE_COUNT];

ui_context_t *ui_context(void) { return &render_context; }
int64_t esp_timer_get_time(void) { return (int64_t)(render_now_ms * 1000U); }
esp_err_t workflow_service_try_get_snapshot(workflow_snapshot_t *out)
{
    if (render_workflow_snapshot_result != ESP_OK) return render_workflow_snapshot_result;
    *out = render_snapshot;
    return ESP_OK;
}
/* ESP_OK normally; a test sets it to make the non-blocking status read fail like a busy mutex. */
esp_err_t render_device_status_result = ESP_OK;
esp_err_t device_status_try_get_snapshot(device_status_snapshot_t *out)
{
    if (render_device_status_result != ESP_OK) return render_device_status_result;
    *out = render_device_status;
    return ESP_OK;
}
bool hw_inventory_required_missing(hw_module_id_t *missing) { (void)missing; return render_required_missing; }
bool hw_inventory_get(hw_module_id_t id, hw_module_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->id = id;
    out->state = render_required_missing ? HW_STATE_FAULT : HW_STATE_PRESENT;
    return true;
}
bool wifi_manager_is_setup_mode(void) { return render_wifi_setup; }
esp_err_t wifi_manager_sta_ip_string(char *dst, size_t capacity)
{
    if (render_wifi_ip) { (void)snprintf(dst, capacity, "192.0.2.123"); return ESP_OK; }
    if (capacity > 0U) dst[0] = '\0';
    return ESP_ERR_INVALID_STATE;
}
esp_err_t seismic_acquisition_get_snapshot(seismic_acquisition_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    out->source_name = "AD7606";
    out->running = true;
    out->sample_rate_hz = 20000U;
    if (render_acquisition_mode == 1U) out->serial_quality = "FAULT";
    if (render_acquisition_mode == 2U) out->serial_pattern = "DIGITAL_INTERFERENCE";
    if (render_acquisition_mode == 3U) return ESP_ERR_INVALID_STATE;
    if (render_acquisition_mode == 4U) out->running = false;
    return ESP_OK;
}
#ifndef UI_NATIVE_FULL_SCREENS
size_t ui_screen_home_count(void) { return UI_HOME_SCREEN_COUNT; }
const char *ui_screen_home_label(size_t index)
{
    static const ui_i18n_key_t keys[] = {
        UI_I18N_KEY_MENU_OBJECTS, UI_I18N_KEY_MENU_MEASUREMENTS, UI_I18N_KEY_MENU_STORAGE,
        UI_I18N_KEY_WIFI_TITLE, UI_I18N_KEY_SYSTEM_TITLE
    };
    return ui_tr(keys[index]);
}
void ui_screen_open_locked(ui_screen_t screen) { render_context.screen = screen; }
void ui_screen_render_locked(ui_screen_t screen) { (void)screen; }
void ui_screen_refresh_locked(ui_screen_t screen) { (void)screen; }
void ui_screen_handle_key_locked(ui_screen_t screen, keypad_key_t key)
{ (void)screen; (void)key; assert(false); }
#endif
esp_err_t seismic_acquisition_start(void) { ++render_acquisition_starts; return render_acquisition_result; }
esp_err_t seismic_acquisition_probe(void) { return render_acquisition_result; }
esp_err_t seismic_acquisition_set_preview(seismic_preview_t preview, bool active)
{
    assert(preview == SEISMIC_PREVIEW_TFT);
    return active ? seismic_acquisition_start() : ESP_OK;
}

/* Native tests inject completed file results explicitly; never run an RTOS task. */
BaseType_t xTaskCreatePinnedToCore(void (*task)(void *), const char *name, uint32_t stack,
    void *arg, unsigned priority, TaskHandle_t *handle, BaseType_t core)
{
    (void)stack; (void)priority; (void)core;
#ifdef UI_NATIVE_FULL_SCREENS
    assert(render_task_count < 2U);
    render_tasks[render_task_count].run = task;
    render_tasks[render_task_count].argument = arg;
    render_tasks[render_task_count].name = name;
    *handle = &render_tasks[render_task_count++];
    return pdPASS;
#else
    (void)task; (void)name; (void)arg; (void)handle;
    return 0;
#endif
}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t timeout)
{
    (void)clear; (void)timeout;
#ifdef UI_NATIVE_FULL_SCREENS
    assert(render_task_running);
    if (!render_task_notified) longjmp(render_task_return, 1);
    render_task_notified = false;
    return 1U;
#else
    assert(false); return 0U;
#endif
}
void xTaskNotifyGive(TaskHandle_t task)
{
    (void)task;
#ifndef UI_NATIVE_FULL_SCREENS
    assert(false);
#endif
}
void vTaskDelay(TickType_t ticks) { (void)ticks; assert(false); }
bool lvgl_port_lock(uint32_t timeout)
{
    (void)timeout;
#ifdef UI_NATIVE_FULL_SCREENS
    if (render_task_running && render_completion_interleave != NULL) {
        void (*interleave)(void) = render_completion_interleave;
        render_completion_interleave = NULL;
        interleave();
    }
#endif
    return true;
}
void lvgl_port_unlock(void) {}

esp_err_t workflow_service_command_async(workflow_command_t command, bool has_generation,
    uint32_t expected_generation, uint32_t *out_seq)
{
    assert(has_generation && expected_generation == render_snapshot.generation);
    assert((render_snapshot.allowed_actions & WORKFLOW_ACTION(command)) != 0U);
    if (render_service_result != ESP_OK) return render_service_result;
    render_last_command = command;
    *out_seq = ++render_command_count;
    return ESP_OK;
}
workflow_command_params_t render_distance_params;
unsigned render_distance_calls;
esp_err_t workflow_service_set_distance_async(const workflow_command_params_t *params, uint32_t *out_seq)
{
    assert(params != NULL && params->has_distance);
    render_distance_params = *params;
    ++render_distance_calls;
    return workflow_service_command_async(WORKFLOW_COMMAND_SET_DISTANCE,
        params->has_generation, params->expected_generation, out_seq);
}
esp_err_t workflow_service_save_calc_material_range_async(const char *material, uint32_t velocity,
    uint32_t sigma, bool range, uint32_t minimum, uint32_t maximum)
{
    (void)material; (void)velocity; (void)sigma; (void)range; (void)minimum; (void)maximum;
    return ESP_OK;
}
bool render_ranging_available;
nrf_topology_ranging_progress_t render_ranging_progress;
esp_err_t nrf_topology_service_ranging_get_progress(nrf_topology_ranging_progress_t *out)
{
    if (!render_ranging_available) { memset(out, 0, sizeof(*out)); return ESP_ERR_INVALID_STATE; }
    *out = render_ranging_progress;
    return ESP_OK;
}
esp_err_t nrf_topology_service_get_sensor_snapshot(uint8_t channel, nrf_remote_sensor_cache_t *out)
{ (void)channel; memset(out, 0, sizeof(*out)); return ESP_ERR_NOT_FOUND; }
esp_err_t nrf_topology_service_get_node_status_snapshot(uint8_t slot, nrf_node_link_status_input_t *out)
{
    if (slot > UI_WORKFLOW_REMOTE_SLOT_COUNT || out == NULL) return ESP_ERR_INVALID_ARG;
    *out = render_node_status[slot];
    return ESP_OK;
}
esp_err_t seismic_recorder_list_sessions(const char *object, seismic_recorder_session_summary_t *out,
    size_t maximum, size_t *count)
{ (void)object; (void)out; (void)maximum; *count = 0U; return ESP_ERR_NOT_FOUND; }
esp_err_t seismic_recorder_read_session(const char *object, const char *session, uint64_t start,
    uint64_t frames, size_t maximum, seismic_recorder_waveform_point_t *out,
    size_t capacity, seismic_recorder_waveform_info_t *info)
{
    (void)object; (void)session; (void)start; (void)frames; (void)maximum;
    (void)out; (void)capacity; (void)info; return ESP_ERR_NOT_FOUND;
}
esp_err_t seismic_recorder_read_session_geometry(const char *object, const char *session,
    object_store_geometry_t *out)
{ (void)object; (void)session; memset(out, 0, sizeof(*out)); return ESP_ERR_NOT_FOUND; }
esp_err_t localization_run_store_list(const char *object, const char *session,
    localization_run_record_t *out, size_t maximum, size_t *count)
{ (void)object; (void)session; (void)out; (void)maximum; *count = 0U; return ESP_ERR_NOT_FOUND; }
esp_err_t localization_run_store_list_picks(const char *object, const char *session,
    localization_pick_record_t *out, size_t maximum, size_t *count)
{ (void)object; (void)session; (void)out; (void)maximum; *count = 0U; return ESP_ERR_NOT_FOUND; }

esp_err_t seismic_buffer_copy_latest(seismic_frame_t *frames, size_t maximum,
    size_t *count, seismic_buffer_info_t *info)
{
    (void)frames; (void)maximum; *count = 0U; memset(info, 0, sizeof(*info));
    return ESP_ERR_INVALID_STATE;
}
esp_err_t seismic_buffer_copy_since(uint64_t *cursor, seismic_frame_t *frames, size_t maximum,
    size_t *count, uint64_t *dropped, seismic_buffer_info_t *info)
{
    (void)cursor; *dropped = 0U;
    return seismic_buffer_copy_latest(frames, maximum, count, info);
}
esp_err_t measurement_service_get_snapshot(measurement_service_snapshot_t *out)
{ *out = render_measurement; return render_measurement_result; }
esp_err_t measurement_service_start_async(void) { return ESP_OK; }
esp_err_t measurement_service_stop_async(void) { return ESP_OK; }
esp_err_t measurement_service_keep_async(void) { return ESP_OK; }
esp_err_t measurement_service_delete_async(void) { return ESP_OK; }

esp_err_t object_service_get_active(object_service_summary_t *out)
{
    if (!render_active_object || render_context.object_count == 0U) return ESP_ERR_NOT_FOUND;
    *out = render_context.objects[0]; return ESP_OK;
}
bool object_service_is_busy(void) { return render_service_busy; }
esp_err_t object_service_create_async(void) { return render_service_result; }
esp_err_t object_service_list_async(void) { ++render_list_calls; return render_service_result; }
esp_err_t object_service_set_active_async(const char *id) { (void)id; ++render_active_calls; return render_service_result; }
esp_err_t object_service_delete_async(const char *id) { (void)id; ++render_delete_calls; return render_service_result; }
esp_err_t object_store_get_delete_scope(const char *id, object_store_delete_scope_t *scope)
{
    (void)id;
    *scope = (object_store_delete_scope_t){.sessions = 999U, .artifacts = 9999U, .localization_runs = 12345U};
    return render_scope_available ? ESP_OK : ESP_ERR_NOT_FOUND;
}
bool measurement_service_snapshot_allows_object_mutation(const measurement_service_snapshot_t *snapshot)
{ return !snapshot->busy && (snapshot->state == MEASUREMENT_SERVICE_LIVE || snapshot->state == MEASUREMENT_SERVICE_ERROR); }
esp_err_t storage_service_info_async(void) { ++render_info_calls; return render_service_result; }
esp_err_t storage_service_erase_data_async(void) { ++render_erase_calls; return render_service_result; }
bool storage_service_is_busy(void) { return render_service_busy; }
bool storage_manager_is_ready(void) { return render_storage_ready; }
bool wifi_manager_setup_client_connected(void) { return render_wifi_client; }
esp_err_t wifi_manager_get_saved_ssid(char *text, size_t size)
{
    (void)snprintf(text, size, "%s", render_wifi_saved ? "Fixture-Network-With-A-Long-SSID" : "");
    return render_wifi_saved ? ESP_OK : ESP_ERR_NOT_FOUND;
}
bool wifi_manager_is_running(void) { return true; }
bool wifi_manager_is_radio_state_uncertain(void) { return false; }
bool wifi_manager_network_events_suppressed(void) { return false; }
bool wifi_manager_sta_has_ip(void) { return render_wifi_ip; }
esp_err_t wifi_manager_enter_setup_mode(void) { ++render_wifi_setup_calls; return render_service_result; }
esp_err_t wifi_manager_connect_saved(void) { return render_service_result; }
esp_err_t wifi_manager_forget_sta_credentials(void) { ++render_wifi_forget_calls; return render_service_result; }
esp_err_t ui_event_dispatcher_queue_locale_save(ui_locale_t locale) { (void)locale; return render_service_result; }
size_t hw_inventory_snapshot(hw_module_info_t *out, size_t capacity)
{
    const size_t count = capacity < HW_MODULE_COUNT ? capacity : HW_MODULE_COUNT;
    memcpy(out, render_modules, count * sizeof(*out)); return count;
}
size_t heap_caps_get_free_size(unsigned caps) { return caps == MALLOC_CAP_INTERNAL ? 123456U : 6543210U; }
const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t app = {.version = "native-ui-all-menus-2026.10.21-long-version"}; return &app;
}
const char *esp_get_idf_version(void) { return "v5.5.4-ui-test"; }
esp_lcd_panel_io_handle_t display_io_handle(void) { return NULL; }
esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io, int command, const void *data, size_t size)
{ (void)io; (void)command; (void)data; (void)size; assert(false); return ESP_FAIL; }
bool storage_bulk_admission_is_protected(void) { return false; }
esp_err_t storage_bulk_admission_run_chunk(esp_err_t (*callback)(void *), void *context)
{ (void)callback; (void)context; assert(false); return ESP_ERR_INVALID_STATE; }
esp_err_t storage_manager_operation_begin(void) { assert(false); return ESP_ERR_INVALID_STATE; }
void storage_manager_operation_end(void) { assert(false); }
