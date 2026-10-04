#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adc_service.h"
#include "app_events.h"
#include "app_storage_boot_ui.h"
#include "board_config.h"
#include "button_interface.h"
#include "button_types.h"
#include "display.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "iot_button.h"
#include "keypad.h"
#include "power_control.h"
#include "sd_card.h"
#include "sd_card_diagnostics.h"
#include "sd_card_format.h"
#include "storage_manager.h"
#include "ui.h"
#include "ui_i18n.h"
#include "ui_i18n_nvs.h"

#include "../../main/app_storage_boot.c"


#define PIPELINE_SCRIPT_CAPACITY 4096U
#define PIPELINE_QUEUE_CAPACITY 8U
#define PIPELINE_QUEUE_ITEM_SIZE 24U
#define PIPELINE_BUTTON_COUNT 5U
#define PIPELINE_BUTTON_PERIOD_US 10000LL
#define PIPELINE_BUTTON_DEBOUNCE_TICKS 6U

typedef struct {
    int raw;
    esp_err_t err;
} scripted_adc_sample_t;

typedef struct {
    const button_driver_t *driver;
    button_cb_t press_down_callback;
    void *user_data;
    uint8_t stable_level;
    unsigned debounce_count;
    bool created;
} pipeline_button_t;

typedef struct fake_queue {
    unsigned item_size;
    unsigned head;
    unsigned count;
    uint8_t items[PIPELINE_QUEUE_CAPACITY][PIPELINE_QUEUE_ITEM_SIZE];
} fake_queue_t;


static scripted_adc_sample_t s_adc_script[PIPELINE_SCRIPT_CAPACITY];
static size_t s_adc_script_count;
static size_t s_adc_script_index;
static int s_last_good_raw = 4095;
static unsigned s_adc_read_calls;
static unsigned s_direct_adc_calls;
static int64_t s_now_us;

static pipeline_button_t s_buttons[PIPELINE_BUTTON_COUNT];
static size_t s_button_count;
static unsigned s_button_delete_count;
static unsigned s_registered_press_count;
static keypad_key_t s_registered_keys[32];
static unsigned s_app_keypad_events;

static fake_queue_t s_queue;
static bool s_queue_created;
static power_control_shutdown_notification_hook_t s_shutdown_hook;
static void *s_shutdown_hook_context;
static bool s_trigger_power_after_format_failure;
static bool s_power_notification_pending;

static sdmmc_card_t s_fake_card;
static esp_err_t s_mount_result = ESP_OK;
static esp_err_t s_probe_result = ESP_FAIL;
static unsigned s_mount_calls;
static unsigned s_probe_calls;
static unsigned s_diagnostic_calls;
static unsigned s_unmount_calls;

static bool s_format_failure_script[8];
static unsigned s_format_failure_count;
static unsigned s_format_start_calls;
static unsigned s_format_status_calls;
static unsigned s_current_format_status_call;
static unsigned s_storage_init_calls;

static bool s_display_ready;
static unsigned s_display_init_calls;
static unsigned s_display_deinit_calls;
static unsigned s_boot_ui_init_calls;
static unsigned s_ui_shutdown_calls;
static unsigned s_power_arm_calls;
static unsigned s_power_wait_calls;
static unsigned s_power_shutdown_calls;
static bool s_poweroff_jump_ready;
static jmp_buf s_poweroff_jump;

static ui_locale_t s_saved_locale = UI_LOCALE_UK;
static char s_last_reason[96];
static char s_last_technical_code[96];
static char s_last_warning[96];
static char s_last_primary_hint[96];
static char s_last_retry_hint[96];
static char s_last_cancel_hint[96];
static char s_last_error_name[48];
static char s_last_boot_error[64];
static unsigned s_recovery_prompt_count;
static unsigned s_failure_prompt_count;
static unsigned s_format_result_count;
static bool s_format_results[8];
static bool s_format_result_formatted[8];
static unsigned s_format_progress[64];
static unsigned s_format_progress_count;
static bool s_current_failure_prompt;
static int64_t s_failure_prompt_diag_times[16];
static unsigned s_failure_prompt_diag_count;
static unsigned s_keypad_press_log_count;
static unsigned s_keypad_transition_log_count;
static unsigned s_keypad_boot_diag_log_count;
static char s_last_keypad_transition[256];
static char s_last_keypad_diag[256];


static void copy_text(char *destination, size_t capacity, const char *source)
{
    if (capacity == 0U) {
        return;
    }
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    (void)snprintf(destination, capacity, "%s", source);
}


void keypad_test_log_capture(const char *tag, const char *format, va_list args)
{
    (void)tag;
    char line[256];
    (void)vsnprintf(line, sizeof(line), format, args);

    if (strncmp(line, "KEYPAD_BUTTON_PRESS", 19U) == 0) {
        ++s_keypad_press_log_count;
        s_current_failure_prompt = false;
    } else if (strncmp(line, "KEYPAD_RAW_TRANSITION", 21U) == 0) {
        ++s_keypad_transition_log_count;
        copy_text(
            s_last_keypad_transition,
            sizeof(s_last_keypad_transition),
            line
        );
    } else if (strncmp(line, "KEYPAD_BOOT_INPUT", 17U) == 0) {
        ++s_keypad_boot_diag_log_count;
        copy_text(s_last_keypad_diag, sizeof(s_last_keypad_diag), line);
        if (
            s_current_failure_prompt &&
            s_failure_prompt_diag_count <
                sizeof(s_failure_prompt_diag_times) /
                    sizeof(s_failure_prompt_diag_times[0])
        ) {
            s_failure_prompt_diag_times[s_failure_prompt_diag_count++] = s_now_us;
        }
    }
}


static void adc_script_clear(void)
{
    s_adc_script_count = 0U;
    s_adc_script_index = 0U;
    s_last_good_raw = 4095;
}


static void adc_script_append(int raw, esp_err_t err, unsigned count)
{
    assert(count <= PIPELINE_SCRIPT_CAPACITY - s_adc_script_count);
    for (unsigned i = 0U; i < count; ++i) {
        s_adc_script[s_adc_script_count++] = (scripted_adc_sample_t){
            .raw = raw,
            .err = err,
        };
    }
    if (err == ESP_OK && count > 0U) {
        s_last_good_raw = raw;
    }
}


static void format_script_set(const bool *failures, unsigned count)
{
    assert(count <= sizeof(s_format_failure_script) /
        sizeof(s_format_failure_script[0]));
    memset(s_format_failure_script, 0, sizeof(s_format_failure_script));
    if (count > 0U) {
        memcpy(s_format_failure_script, failures, count * sizeof(bool));
    }
    s_format_failure_count = count;
    s_format_start_calls = 0U;
    s_format_status_calls = 0U;
    s_current_format_status_call = 0U;
    s_trigger_power_after_format_failure = false;
    s_power_notification_pending = false;
}


static void prepare_keypad(void)
{
    keypad_stop_event_monitor();
    assert(keypad_init() == ESP_OK);
}


static app_storage_boot_policy_input_t write_probe_failure(void)
{
    return (app_storage_boot_policy_input_t){
        .stage = APP_STORAGE_BOOT_STAGE_WRITE_PROBE,
        .err = ESP_OK,
        .fresult = -1,
        .probe_result = ESP_FAIL,
        .probe_stage_name = "READ",
        .probe_errno = 2,
        .storage_manager_init_result = ESP_OK,
    };
}


esp_err_t adc_service_init(void)
{
    return ESP_OK;
}


adc_unit_t adc_service_unit(void)
{
    return 1;
}


esp_err_t adc_service_configure_gpio(
    gpio_num_t gpio,
    adc_atten_t attenuation,
    adc_bitwidth_t bitwidth,
    adc_channel_t *channel
)
{
    assert(gpio == BOARD_KEYPAD_GPIO);
    assert(attenuation == ADC_ATTEN_DB_12);
    assert(bitwidth == ADC_BITWIDTH_DEFAULT);
    assert(channel != NULL);
    *channel = 7;
    return ESP_OK;
}


esp_err_t adc_service_try_read_average_raw(
    adc_channel_t channel,
    size_t sample_count,
    int *raw_value
)
{
    assert(channel == 7);
    assert(sample_count == 16U);
    assert(raw_value != NULL);
    ++s_adc_read_calls;

    scripted_adc_sample_t sample = {
        .raw = s_last_good_raw,
        .err = ESP_OK,
    };
    if (s_adc_script_index < s_adc_script_count) {
        sample = s_adc_script[s_adc_script_index++];
    }
    if (sample.err == ESP_OK) {
        *raw_value = sample.raw;
        s_last_good_raw = sample.raw;
    }
    return sample.err;
}


esp_err_t adc_service_read_average_raw(
    adc_channel_t channel,
    size_t sample_count,
    int *raw_value
)
{
    return adc_service_try_read_average_raw(channel, sample_count, raw_value);
}


esp_err_t adc_service_read_raw(adc_channel_t channel, int *raw_value)
{
    return adc_service_read_average_raw(channel, 1U, raw_value);
}


esp_err_t adc_oneshot_read(
    adc_oneshot_unit_handle_t handle,
    adc_channel_t channel,
    int *raw_value
)
{
    (void)handle;
    (void)channel;
    (void)raw_value;
    ++s_direct_adc_calls;
    return ESP_ERR_INVALID_STATE;
}


int64_t esp_timer_get_time(void)
{
    return s_now_us;
}


esp_err_t iot_button_create(
    const button_config_t *config,
    const button_driver_t *driver,
    button_handle_t *ret_button
)
{
    assert(config != NULL);
    assert(config->short_press_time == 60U);
    assert(config->long_press_time == 1500U);
    assert(driver != NULL && driver->get_key_level != NULL);
    assert(ret_button != NULL);
    assert(s_button_count < PIPELINE_BUTTON_COUNT);

    pipeline_button_t *button = &s_buttons[s_button_count++];
    *button = (pipeline_button_t){
        .driver = driver,
        .stable_level = BUTTON_INACTIVE,
        .created = true,
    };
    *ret_button = (button_handle_t)(void *)button;
    return ESP_OK;
}


esp_err_t iot_button_register_cb(
    button_handle_t btn_handle,
    button_event_t event,
    button_event_args_t *event_args,
    button_cb_t callback,
    void *user_data
)
{
    assert(btn_handle != NULL);
    assert(event == BUTTON_PRESS_DOWN);
    assert(event_args == NULL);
    assert(callback != NULL);
    assert(user_data != NULL);

    pipeline_button_t *button = (pipeline_button_t *)(void *)btn_handle;
    button->press_down_callback = callback;
    button->user_data = user_data;
    return ESP_OK;
}


esp_err_t iot_button_delete(button_handle_t btn_handle)
{
    assert(btn_handle != NULL);
    pipeline_button_t *button = (pipeline_button_t *)(void *)btn_handle;
    button->created = false;
    ++s_button_delete_count;
    if (s_button_delete_count == PIPELINE_BUTTON_COUNT) {
        memset(s_buttons, 0, sizeof(s_buttons));
        s_button_count = 0U;
        s_button_delete_count = 0U;
    }
    return ESP_OK;
}


static void pipeline_button_tick(void)
{
    s_now_us += PIPELINE_BUTTON_PERIOD_US;
    for (size_t i = 0U; i < s_button_count; ++i) {
        pipeline_button_t *button = &s_buttons[i];
        if (!button->created) {
            continue;
        }

        const uint8_t level = button->driver->get_key_level(
            (button_driver_t *)button->driver
        );
        if (level != button->stable_level) {
            if (++button->debounce_count >= PIPELINE_BUTTON_DEBOUNCE_TICKS) {
                button->stable_level = level;
                button->debounce_count = 0U;
                if (level == BUTTON_ACTIVE && button->press_down_callback != NULL) {
                    assert(s_registered_press_count <
                        sizeof(s_registered_keys) / sizeof(s_registered_keys[0]));
                    s_registered_keys[s_registered_press_count++] =
                        (keypad_key_t)(uintptr_t)button->user_data;
                    button->press_down_callback(button, button->user_data);
                }
            }
        } else {
            button->debounce_count = 0U;
        }
    }
}


static void queue_pop(fake_queue_t *queue, void *item)
{
    assert(queue->count > 0U);
    const unsigned index = queue->head;
    memcpy(item, queue->items[index], queue->item_size);
    queue->head = (queue->head + 1U) % PIPELINE_QUEUE_CAPACITY;
    --queue->count;
}


QueueHandle_t xQueueCreate(unsigned length, unsigned item_size)
{
    assert(length == 5U);
    assert(item_size > 0U && item_size <= PIPELINE_QUEUE_ITEM_SIZE);
    memset(&s_queue, 0, sizeof(s_queue));
    s_queue.item_size = item_size;
    s_queue_created = true;
    return &s_queue;
}


BaseType_t xQueueReset(QueueHandle_t queue)
{
    assert(queue == &s_queue && s_queue_created);
    queue->head = 0U;
    queue->count = 0U;
    return pdTRUE;
}


BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait_ticks)
{
    (void)wait_ticks;
    assert(queue == &s_queue && s_queue_created);
    assert(item != NULL);
    if (queue->count == PIPELINE_QUEUE_CAPACITY) {
        return pdFALSE;
    }
    const unsigned tail = (queue->head + queue->count) % PIPELINE_QUEUE_CAPACITY;
    memcpy(queue->items[tail], item, queue->item_size);
    ++queue->count;
    return pdTRUE;
}


BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait_ticks)
{
    (void)wait_ticks;
    assert(queue == &s_queue && s_queue_created);
    assert(item != NULL);
    if (queue->count > 0U) {
        queue_pop(queue, item);
        return pdTRUE;
    }

    for (unsigned tick = 0U; tick < 700U; ++tick) {
        if (s_power_notification_pending) {
            assert(s_shutdown_hook != NULL);
            s_power_notification_pending = false;
            s_shutdown_hook(s_shutdown_hook_context);
        } else {
            pipeline_button_tick();
        }
        if (queue->count > 0U) {
            queue_pop(queue, item);
            return pdTRUE;
        }
    }

    fputs("pipeline input script ended before boot UI decision\n", stderr);
    abort();
}


void vTaskDelay(TickType_t ticks)
{
    const unsigned poll_count = (unsigned)(ticks / 10U);
    for (unsigned i = 0U; i < poll_count; ++i) {
        pipeline_button_tick();
    }
}


esp_err_t app_events_post(app_event_type_t type, int32_t value)
{
    (void)value;
    assert(type == APP_EVENT_KEYPAD);
    ++s_app_keypad_events;
    return ESP_OK;
}


bool display_is_ready(void)
{
    return s_display_ready;
}


esp_err_t display_init(void)
{
    ++s_display_init_calls;
    s_display_ready = true;
    return ESP_OK;
}


esp_err_t display_set_enabled(bool enabled)
{
    (void)enabled;
    return ESP_OK;
}


void display_deinit(void)
{
    ++s_display_deinit_calls;
    s_display_ready = false;
}


esp_err_t ui_boot_init(void)
{
    ++s_boot_ui_init_calls;
    return ESP_OK;
}


void ui_boot_show_error(const char *message)
{
    copy_text(s_last_boot_error, sizeof(s_last_boot_error), message);
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
    ++s_recovery_prompt_count;
    copy_text(s_last_reason, sizeof(s_last_reason), reason);
    copy_text(s_last_technical_code, sizeof(s_last_technical_code), technical_code);
    copy_text(s_last_warning, sizeof(s_last_warning), warning);
    copy_text(s_last_primary_hint, sizeof(s_last_primary_hint), primary_hint);
    copy_text(s_last_retry_hint, sizeof(s_last_retry_hint), retry_hint);
    copy_text(s_last_cancel_hint, sizeof(s_last_cancel_hint), cancel_hint);
    s_current_failure_prompt =
        warning != NULL &&
        strcmp(warning, ui_tr(UI_I18N_KEY_BOOT_FORMAT_FAILED)) == 0;
    if (s_current_failure_prompt) {
        ++s_failure_prompt_count;
        s_failure_prompt_diag_count = 0U;
    }
}


void ui_boot_show_format_progress(unsigned percent)
{
    assert(s_format_progress_count <
        sizeof(s_format_progress) / sizeof(s_format_progress[0]));
    s_format_progress[s_format_progress_count++] = percent;
}


void ui_boot_show_recovery_result(
    bool success,
    bool formatted,
    const char *error_name
)
{
    assert(s_format_result_count <
        sizeof(s_format_results) / sizeof(s_format_results[0]));
    s_format_results[s_format_result_count] = success;
    s_format_result_formatted[s_format_result_count] = formatted;
    ++s_format_result_count;
    copy_text(s_last_error_name, sizeof(s_last_error_name), error_name);
    s_current_failure_prompt = false;
}


void ui_shutdown(void)
{
    ++s_ui_shutdown_calls;
}


ui_locale_t ui_i18n_nvs_load_locale(void)
{
    return s_saved_locale;
}


bool ui_i18n_nvs_save_locale(ui_locale_t locale)
{
    s_saved_locale = locale;
    return true;
}


esp_err_t power_control_arm_shutdown_button(void)
{
    ++s_power_arm_calls;
    return ESP_OK;
}


esp_err_t power_control_set_shutdown_notification_hook(
    power_control_shutdown_notification_hook_t hook,
    void *user_context
)
{
    s_shutdown_hook = hook;
    s_shutdown_hook_context = user_context;
    return ESP_OK;
}


void power_control_wait_for_shutdown_request(void)
{
    ++s_power_wait_calls;
}


void power_control_shutdown(void)
{
    ++s_power_shutdown_calls;
}


void app_poweroff_idle_forever(void)
{
    if (s_poweroff_jump_ready) {
        longjmp(s_poweroff_jump, 1);
    }
    abort();
}


esp_err_t sd_card_mount(void)
{
    return s_mount_result;
}


esp_err_t sd_card_mount_recovery(sd_card_mount_report_t *report)
{
    ++s_mount_calls;
    if (report != NULL) {
        *report = (sd_card_mount_report_t){
            .stage = s_mount_result == ESP_OK
                ? SD_CARD_MOUNT_STAGE_NONE
                : SD_CARD_MOUNT_STAGE_FS_MOUNT,
            .err = s_mount_result,
            .fresult = -1,
            .health = s_mount_result == ESP_OK
                ? SD_CARD_HEALTH_READY
                : SD_CARD_HEALTH_FILESYSTEM_UNAVAILABLE,
        };
    }
    return s_mount_result;
}


sdmmc_card_t *sd_card_get_card_handle(void)
{
    return &s_fake_card;
}


esp_err_t sd_card_unmount(void)
{
    ++s_unmount_calls;
    return ESP_OK;
}


esp_err_t sd_card_diagnostics_run_write_read_test(void)
{
    ++s_diagnostic_calls;
    return ESP_OK;
}


sd_card_probe_result_t sd_card_write_readback_probe(void)
{
    ++s_probe_calls;
    return (sd_card_probe_result_t){
        .stage = s_probe_result == ESP_OK
            ? SD_CARD_PROBE_STAGE_COMPLETE
            : SD_CARD_PROBE_STAGE_READ,
        .err = s_probe_result,
        .sys_errno = s_probe_result == ESP_OK ? 0 : 2,
    };
}


const char *sd_card_probe_stage_name(sd_card_probe_stage_t stage)
{
    switch (stage) {
        case SD_CARD_PROBE_STAGE_READ: return "READ";
        case SD_CARD_PROBE_STAGE_COMPLETE: return "COMPLETE";
        default: return "UNKNOWN";
    }
}


esp_err_t sd_card_format_fat32_task_start(sdmmc_card_t *card, bool confirmed)
{
    assert(card == &s_fake_card);
    assert(confirmed);
    assert(s_format_start_calls < s_format_failure_count);
    ++s_format_start_calls;
    s_current_format_status_call = 0U;
    return ESP_OK;
}


esp_err_t sd_card_format_fat32_task_get_status(sd_card_format_status_t *out_status)
{
    assert(out_status != NULL);
    assert(s_format_start_calls > 0U);
    ++s_format_status_calls;
    if (s_current_format_status_call++ == 0U) {
        *out_status = (sd_card_format_status_t){
            .state = SD_CARD_FORMAT_STATE_RUNNING,
            .percent = 40U,
            .err = ESP_OK,
            .fresult = -1,
        };
        return ESP_OK;
    }

    const bool failed = s_format_failure_script[s_format_start_calls - 1U];
    *out_status = (sd_card_format_status_t){
        .state = failed
            ? SD_CARD_FORMAT_STATE_FAILED
            : SD_CARD_FORMAT_STATE_DONE,
        .percent = failed ? 72U : 100U,
        .err = failed ? ESP_FAIL : ESP_OK,
        .fresult = failed ? 14 : 0,
    };
    if (failed && s_trigger_power_after_format_failure) {
        s_power_notification_pending = true;
        s_trigger_power_after_format_failure = false;
    }
    return ESP_OK;
}


esp_err_t sd_card_format_fat32_task_stop(void)
{
    return ESP_OK;
}


esp_err_t storage_manager_init(void)
{
    ++s_storage_init_calls;
    return ESP_OK;
}


static void test_first_press_after_3600_idle_completes_pipeline(void)
{
    prepare_keypad();
    adc_script_clear();
    format_script_set((const bool[]){false}, 1U);
    s_mount_result = ESP_OK;
    s_probe_result = ESP_FAIL;

    /* A floating input initially looks exactly like OK. It is quarantined
     * until three consecutive high-idle samples arrive. The held OK then
     * crosses the real adapter and six-tick Button debounce despite one ADC
     * mutex timeout. */
    adc_script_append(2873, ESP_OK, 8U);
    adc_script_append(3600, ESP_OK, 3U);
    adc_script_append(2873, ESP_OK, 2U);
    adc_script_append(0, ESP_ERR_TIMEOUT, 1U);
    adc_script_append(2873, ESP_OK, 10U);
    adc_script_append(3600, ESP_OK, 80U);

    const unsigned presses_before = s_registered_press_count;
    app_storage_boot_run_diagnostic();

    assert(s_mount_calls > 0U);
    assert(s_probe_calls > 0U);
    assert(s_diagnostic_calls == 1U);
    assert(s_format_start_calls == 1U);
    assert(s_format_status_calls == 2U);
    assert(s_format_result_count == 1U);
    assert(s_format_results[0]);
    assert(s_format_result_formatted[0]);
    assert(strstr(s_last_technical_code, "READ") != NULL);
    assert(s_registered_press_count == presses_before + 1U);
    assert(s_registered_keys[presses_before] == KEYPAD_KEY_OK);
    assert(s_keypad_press_log_count >= 1U);
    assert(s_keypad_transition_log_count >= 1U);
    assert(strstr(s_last_keypad_transition, "raw=") != NULL);
    assert(strstr(s_last_keypad_transition, "decoded=") != NULL);
    assert(strstr(s_last_keypad_transition, "release_armed=") != NULL);
    assert(s_app_keypad_events == 0U);
    assert(s_direct_adc_calls == 0U);
    assert(s_format_progress_count >= 2U);
    assert(s_format_progress[s_format_progress_count - 1U] == 100U);
    assert(strcmp(
        s_last_cancel_hint,
        ui_tr(UI_I18N_KEY_BOOT_RECOVERY_CANCEL_HINT)
    ) == 0);
}


static void test_failed_format_shows_retry_then_second_ok_succeeds(void)
{
    prepare_keypad();
    assert(ui_i18n_get_locale() == UI_LOCALE_UK);
    adc_script_clear();
    format_script_set((const bool[]){true, false}, 2U);

    /* This prompt starts at each requested upper-idle boundary across the
     * two attempts. A non-timeout ADC error during the first press must keep
     * the cached active level long enough to finish Button debounce. */
    adc_script_append(3800, ESP_OK, 3U);
    adc_script_append(2873, ESP_OK, 2U);
    adc_script_append(0, ESP_FAIL, 1U);
    adc_script_append(2873, ESP_OK, 10U);
    adc_script_append(4095, ESP_OK, 230U);
    adc_script_append(2873, ESP_OK, 12U);
    adc_script_append(4095, ESP_OK, 80U);

    const unsigned presses_before = s_registered_press_count;
    const bool recovered = app_storage_boot_recover(write_probe_failure());

    assert(recovered);
    assert(s_format_start_calls == 2U);
    assert(s_format_status_calls == 4U);
    assert(s_format_result_count == 3U);
    assert(!s_format_results[1]);
    assert(s_format_results[2]);
    assert(s_failure_prompt_count == 1U);
    assert(strcmp(s_last_warning, ui_tr(UI_I18N_KEY_BOOT_FORMAT_FAILED)) == 0);
    assert(strcmp(
        s_last_primary_hint,
        ui_tr(UI_I18N_KEY_BOOT_FORMAT_RETRY_HINT)
    ) == 0);
    assert(strcmp(
        s_last_retry_hint,
        ui_tr(UI_I18N_KEY_BOOT_FORMAT_POWER_HINT)
    ) == 0);
    assert(s_registered_press_count == presses_before + 2U);
    assert(s_registered_keys[presses_before] == KEYPAD_KEY_OK);
    assert(s_registered_keys[presses_before + 1U] == KEYPAD_KEY_OK);
    assert(s_failure_prompt_diag_count >= 2U);
    assert(
        s_failure_prompt_diag_times[1] - s_failure_prompt_diag_times[0]
        >= 1900000LL
    );
    assert(strstr(s_last_keypad_diag, "raw=") != NULL);
    assert(strstr(s_last_keypad_diag, "decoded=") != NULL);
    assert(strstr(s_last_keypad_diag, "release_armed=") != NULL);
    assert(s_format_progress_count >= 4U);
    assert(s_format_progress[s_format_progress_count - 1U] == 100U);
}


static void test_failed_format_power_notification_cancels_and_powers_off(void)
{
    prepare_keypad();
    ui_i18n_set_locale(UI_LOCALE_EN);
    assert(ui_i18n_get_locale() == UI_LOCALE_EN);
    adc_script_clear();
    format_script_set((const bool[]){true}, 1U);
    s_trigger_power_after_format_failure = true;
    adc_script_append(4095, ESP_OK, 3U);
    adc_script_append(2873, ESP_OK, 12U);
    adc_script_append(4095, ESP_OK, 80U);
    s_mount_result = ESP_OK;
    s_probe_result = ESP_FAIL;

    s_poweroff_jump_ready = true;
    if (setjmp(s_poweroff_jump) == 0) {
        app_storage_boot_run_diagnostic();
        assert(false);
    }
    s_poweroff_jump_ready = false;

    assert(s_mount_calls > 0U);
    assert(s_probe_calls > 0U);
    assert(s_format_start_calls == 1U);
    assert(s_format_result_count == 4U);
    assert(!s_format_results[s_format_result_count - 1U]);
    assert(s_failure_prompt_count == 2U);
    assert(strcmp(
        s_last_primary_hint,
        ui_tr(UI_I18N_KEY_BOOT_FORMAT_RETRY_HINT)
    ) == 0);
    assert(strcmp(s_last_primary_hint, "OK: RETRY FORMAT") == 0);
    assert(strcmp(
        s_last_retry_hint,
        ui_tr(UI_I18N_KEY_BOOT_FORMAT_POWER_HINT)
    ) == 0);
    assert(s_power_wait_calls == 1U);
    assert(s_power_shutdown_calls == 1U);
    assert(s_unmount_calls == 1U);
    assert(s_ui_shutdown_calls == 1U);
    assert(s_display_deinit_calls == 1U);
    assert(s_app_keypad_events == 0U);
}


int main(void)
{
    s_saved_locale = UI_LOCALE_UK;
    app_storage_boot_ui_register();

    test_first_press_after_3600_idle_completes_pipeline();
    test_failed_format_shows_retry_then_second_ok_succeeds();
    test_failed_format_power_notification_cancels_and_powers_off();

    keypad_stop_event_monitor();
    puts("APP_STORAGE_BOOT_KEYPAD_PIPELINE_PASS");
    return 0;
}
