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
#include "freertos/semphr.h"
#include "hw_inventory.h"
#include "iot_button.h"
#include "keypad.h"
#include "../../components/keypad/keypad_decode.h"
#include "power_control.h"
#include "sd_card.h"
#include "sd_card_diagnostics.h"
#include "sd_card_format.h"
#include "sd_card_info.h"
#include "storage_manager.h"
#include "ui.h"
#include "ui_i18n.h"
#include "ui_i18n_nvs.h"

#include "../../main/app_storage_boot.c"


#define PIPELINE_SCRIPT_CAPACITY 8192U
#define PIPELINE_QUEUE_CAPACITY 8U
#define PIPELINE_QUEUE_ITEM_SIZE 24U
#define PIPELINE_BUTTON_COUNT 5U
#define PIPELINE_BUTTON_PERIOD_US 10000LL
#define PIPELINE_QUEUE_IDLE_TICK_LIMIT 7000U
#define PIPELINE_DIAG_RECORD_CAPACITY 4096U
#define FIELD_REPLAY_TICK_COUNT 5589U

typedef enum {
    PIPELINE_DIAG_TRANSITION,
    PIPELINE_DIAG_BOOT_INPUT,
    PIPELINE_DIAG_IDLE_INVALID,
    PIPELINE_DIAG_BUTTON_PRESS,
} pipeline_diag_kind_t;

typedef struct {
    pipeline_diag_kind_t kind;
    int64_t time_us;
    int raw;
    int needed_raw;
    unsigned release_armed;
    bool raw_valid;
    char from[8];
    char to[8];
    char decoded[8];
    char line[256];
} pipeline_diag_record_t;

typedef struct {
    int raw;
    esp_err_t err;
} scripted_adc_sample_t;

struct pipeline_timer {
    esp_timer_create_args_t args;
    bool created;
    bool running;
};

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
static unsigned s_adc_conversion_calls;
static unsigned s_adc_discard_calls;
static unsigned s_adc_useful_calls;
static unsigned s_adc_conversion_in_group;
static scripted_adc_sample_t s_current_adc_sample;
static bool s_adc_mutex_locked;
static bool s_adc_configured;
static int s_adc_unit_marker;
static int s_adc_mutex_marker;
static size_t s_cancel_at_adc_index;
static int64_t s_now_us;
static void (*s_pipeline_tick_hook)(void);

static struct pipeline_timer s_button_timer;
static unsigned s_registered_press_count;
static keypad_key_t s_registered_keys[32];
static unsigned s_app_keypad_events;

static fake_queue_t s_queue;
static bool s_queue_created;
static unsigned s_queue_send_calls;
static power_control_shutdown_notification_hook_t s_shutdown_hook;
static void *s_shutdown_hook_context;
static bool s_trigger_power_after_format_failure;
static bool s_power_notification_pending;

static sdmmc_card_t s_fake_card;
static esp_err_t s_mount_result = ESP_OK;
static esp_err_t s_probe_result = ESP_FAIL;
static int s_mount_fresult = APP_STORAGE_BOOT_FR_NO_FILESYSTEM;
static uint64_t s_card_capacity_bytes = 32ULL * 1000U * 1000U * 1000U;
static unsigned s_mount_calls;
static unsigned s_probe_calls;
static unsigned s_diagnostic_calls;
static unsigned s_unmount_calls;

static bool s_format_failure_script[8];
static unsigned s_format_failure_count;
static unsigned s_format_start_calls;
static bool s_fail_next_format_start;
static bool s_fail_next_format_status;
static unsigned s_format_start_errors;
static unsigned s_format_status_errors;
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
static unsigned s_continue_without_storage_count;
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
static unsigned s_keypad_idle_invalid_log_count;
static char s_last_keypad_transition[256];
static char s_last_keypad_diag[256];
static char s_last_keypad_idle_invalid[256];
static pipeline_diag_record_t s_keypad_diag_records[PIPELINE_DIAG_RECORD_CAPACITY];
static size_t s_keypad_diag_record_count;
static unsigned s_boot_passed_log_count;
static bool s_field_replay_snapshot_pending;
static int s_field_replay_max_raw;
static unsigned s_field_replay_transitions;
static unsigned s_field_replay_boot_input;
static unsigned s_field_replay_idle_invalid;
static unsigned s_field_replay_presses;
static size_t s_field_replay_snapshot_record_end;

static void field_replay_snapshot_hook(void);


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

    if (strcmp(line, "Isolated SD startup check passed") == 0) {
        ++s_boot_passed_log_count;
    }

    pipeline_diag_record_t *record = NULL;
    if (
        strncmp(line, "KEYPAD_BUTTON_PRESS", 19U) == 0 ||
        strncmp(line, "KEYPAD_RAW_TRANSITION", 21U) == 0 ||
        strncmp(line, "KEYPAD_BOOT_INPUT", 17U) == 0 ||
        strncmp(line, "KEYPAD_IDLE_LEVEL_INVALID", 25U) == 0
    ) {
        assert(s_keypad_diag_record_count < PIPELINE_DIAG_RECORD_CAPACITY);
        record = &s_keypad_diag_records[s_keypad_diag_record_count++];
        *record = (pipeline_diag_record_t){.time_us = s_now_us};
        copy_text(record->line, sizeof(record->line), line);
    }

    if (strncmp(line, "KEYPAD_BUTTON_PRESS", 19U) == 0) {
        assert(record != NULL);
        record->kind = PIPELINE_DIAG_BUTTON_PRESS;
        ++s_keypad_press_log_count;
        assert(s_registered_press_count <
            sizeof(s_registered_keys) / sizeof(s_registered_keys[0]));
        char name[16];
        int key = 0;
        assert(sscanf(line, "KEYPAD_BUTTON_PRESS key=%15s value=%d", name, &key) == 2);
        assert(key >= KEYPAD_KEY_LEFT && key <= KEYPAD_KEY_OK);
        assert(strcmp(name, keypad_key_name((keypad_key_t)key)) == 0);
        s_registered_keys[s_registered_press_count++] = (keypad_key_t)key;
        s_current_failure_prompt = false;
    } else if (strncmp(line, "KEYPAD_RAW_TRANSITION", 21U) == 0) {
        assert(record != NULL);
        record->kind = PIPELINE_DIAG_TRANSITION;
        ++s_keypad_transition_log_count;
        assert(sscanf(
            line,
            "KEYPAD_RAW_TRANSITION raw=%d from=%7s to=%7s decoded=%7s release_armed=%u",
            &record->raw,
            record->from,
            record->to,
            record->decoded,
            &record->release_armed
        ) == 5);
        record->raw_valid = true;
        copy_text(
            s_last_keypad_transition,
            sizeof(s_last_keypad_transition),
            line
        );
    } else if (strncmp(line, "KEYPAD_BOOT_INPUT", 17U) == 0) {
        assert(record != NULL);
        record->kind = PIPELINE_DIAG_BOOT_INPUT;
        ++s_keypad_boot_diag_log_count;
        if (sscanf(
                line,
                "KEYPAD_BOOT_INPUT raw=%d decoded=%7s release_armed=%u",
                &record->raw,
                record->decoded,
                &record->release_armed
            ) == 3) {
            record->raw_valid = true;
        }
        copy_text(s_last_keypad_diag, sizeof(s_last_keypad_diag), line);
        if (
            s_current_failure_prompt &&
            s_failure_prompt_diag_count <
                sizeof(s_failure_prompt_diag_times) /
                    sizeof(s_failure_prompt_diag_times[0])
        ) {
            s_failure_prompt_diag_times[s_failure_prompt_diag_count++] = s_now_us;
        }
    } else if (strncmp(line, "KEYPAD_IDLE_LEVEL_INVALID", 25U) == 0) {
        assert(record != NULL);
        record->kind = PIPELINE_DIAG_IDLE_INVALID;
        ++s_keypad_idle_invalid_log_count;
        assert(sscanf(
            line,
            "KEYPAD_IDLE_LEVEL_INVALID raw=%d need>=%d:",
            &record->raw,
            &record->needed_raw
        ) == 2);
        record->raw_valid = true;
        copy_text(
            s_last_keypad_idle_invalid,
            sizeof(s_last_keypad_idle_invalid),
            line
        );
    }
}


static void clear_keypad_diag_capture(void)
{
    s_keypad_press_log_count = 0U;
    s_keypad_transition_log_count = 0U;
    s_keypad_boot_diag_log_count = 0U;
    s_keypad_idle_invalid_log_count = 0U;
    s_keypad_diag_record_count = 0U;
    s_failure_prompt_diag_count = 0U;
    s_last_keypad_transition[0] = '\0';
    s_last_keypad_diag[0] = '\0';
    s_last_keypad_idle_invalid[0] = '\0';
}


static void adc_script_clear(void)
{
    s_adc_script_count = 0U;
    s_adc_script_index = 0U;
    s_cancel_at_adc_index = 0U;
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


static void adc_script_prepend(int raw, esp_err_t err, unsigned count)
{
    assert(count <= PIPELINE_SCRIPT_CAPACITY - s_adc_script_count);
    const size_t remaining = s_adc_script_count - s_adc_script_index;
    memmove(
        &s_adc_script[s_adc_script_index + count],
        &s_adc_script[s_adc_script_index],
        remaining * sizeof(s_adc_script[0])
    );
    for (unsigned i = 0U; i < count; ++i) {
        s_adc_script[s_adc_script_index + i] = (scripted_adc_sample_t){
            .raw = raw,
            .err = err,
        };
    }
    s_adc_script_count += count;
    if (s_cancel_at_adc_index > 0U && s_cancel_at_adc_index >= s_adc_script_index) {
        s_cancel_at_adc_index += count;
    }
}


static sd_card_mount_stage_t pipeline_mount_stage_for_result(esp_err_t err)
{
    if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_RESPONSE) {
        return SD_CARD_MOUNT_STAGE_CARD_INIT;
    }

    return err == ESP_OK
        ? SD_CARD_MOUNT_STAGE_NONE
        : SD_CARD_MOUNT_STAGE_FS_MOUNT;
}


static bool pipeline_mount_failure_is_retryable(
    sd_card_mount_stage_t stage,
    esp_err_t err,
    int fresult
)
{
    if (stage == SD_CARD_MOUNT_STAGE_FS_MOUNT) {
        return fresult == 1  /* FR_DISK_ERR */ ||
            fresult == 3     /* FR_NOT_READY */ ||
            fresult == 15;   /* FR_TIMEOUT */
    }

    return stage == SD_CARD_MOUNT_STAGE_HOST_INIT ||
        stage == SD_CARD_MOUNT_STAGE_CARD_INIT ||
        err == ESP_ERR_TIMEOUT ||
        err == ESP_ERR_INVALID_RESPONSE;
}


/* Samples consumed by the boot-time delays before the recovery prompt: the SD
 * power-up delay and retry gaps only for failures that will be retried. Call
 * after s_mount_result and s_mount_fresult are set. */
static void prepend_powerup_samples(int raw)
{
    unsigned poll_count = (unsigned)(
        pdMS_TO_TICKS(APP_STORAGE_BOOT_SD_POWERUP_DELAY_MS) / 10U
    );
    const sd_card_mount_stage_t stage =
        pipeline_mount_stage_for_result(s_mount_result);
    const int fresult = stage == SD_CARD_MOUNT_STAGE_FS_MOUNT
        ? s_mount_fresult
        : -1;
    if (s_mount_result != ESP_OK && pipeline_mount_failure_is_retryable(
            stage,
            s_mount_result,
            fresult
        )) {
        poll_count += 2U * (150U / 10U);
    }
    adc_script_prepend(raw, ESP_OK, poll_count);
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
    s_fail_next_format_start = false;
    s_fail_next_format_status = false;
    s_format_start_errors = 0U;
    s_format_status_errors = 0U;
}


static void prepare_keypad(void)
{
    keypad_stop_event_monitor();
    clear_keypad_diag_capture();
    s_pipeline_tick_hook = NULL;
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


static app_storage_boot_policy_input_t format_eligible_mount_failure(void)
{
    return (app_storage_boot_policy_input_t){
        .stage = APP_STORAGE_BOOT_STAGE_FS_MOUNT,
        .err = ESP_FAIL,
        .fresult = APP_STORAGE_BOOT_FR_NO_FILESYSTEM,
        .probe_result = ESP_OK,
        .storage_manager_init_result = ESP_OK,
        .card_capacity_bytes = 32ULL * 1000U * 1000U * 1000U,
    };
}


esp_err_t adc_oneshot_new_unit(
    const adc_oneshot_unit_init_cfg_t *config,
    adc_oneshot_unit_handle_t *handle
)
{
    assert(config != NULL && handle != NULL);
    assert(config->unit_id == ADC_UNIT_1);
    assert(config->ulp_mode == ADC_ULP_MODE_DISABLE);
    *handle = &s_adc_unit_marker;
    return ESP_OK;
}


esp_err_t adc_oneshot_io_to_channel(int gpio, adc_unit_t *unit, adc_channel_t *channel)
{
    assert(gpio == BOARD_KEYPAD_GPIO);
    assert(unit != NULL && channel != NULL);
    *unit = ADC_UNIT_1;
    *channel = 7;
    return ESP_OK;
}


esp_err_t adc_oneshot_config_channel(
    adc_oneshot_unit_handle_t handle,
    adc_channel_t channel,
    const adc_oneshot_chan_cfg_t *config
)
{
    assert(handle == &s_adc_unit_marker && channel == 7);
    assert(config != NULL);
    assert(config->atten == ADC_ATTEN_DB_12);
    assert(config->bitwidth == ADC_BITWIDTH_DEFAULT);
    s_adc_configured = true;
    return ESP_OK;
}


SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return &s_adc_mutex_marker;
}


void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    assert(semaphore == &s_adc_mutex_marker && !s_adc_mutex_locked);
}


BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks)
{
    assert(semaphore == &s_adc_mutex_marker && !s_adc_mutex_locked);
    assert(ticks == 0U); /* Timer path must never wait for the ADC broker. */
    ++s_adc_read_calls;
    s_current_adc_sample = (scripted_adc_sample_t){
        .raw = s_last_good_raw,
        .err = ESP_OK,
    };
    if (s_adc_script_index < s_adc_script_count) {
        s_current_adc_sample = s_adc_script[s_adc_script_index++];
    }
    if (s_current_adc_sample.err == ESP_ERR_TIMEOUT) {
        return pdFALSE;
    }
    s_adc_mutex_locked = true;
    s_adc_conversion_in_group = 0U;
    return pdTRUE;
}


BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    assert(semaphore == &s_adc_mutex_marker && s_adc_mutex_locked);
    assert(s_adc_conversion_in_group ==
        (s_current_adc_sample.err == ESP_OK ? 17U : 1U));
    s_adc_mutex_locked = false;
    return pdTRUE;
}


esp_err_t adc_oneshot_read(
    adc_oneshot_unit_handle_t handle,
    adc_channel_t channel,
    int *raw_value
)
{
    assert(handle == &s_adc_unit_marker && channel == 7);
    assert(s_adc_configured && s_adc_mutex_locked && raw_value != NULL);
    ++s_adc_conversion_calls;
    ++s_adc_conversion_in_group;
    if (s_current_adc_sample.err != ESP_OK) {
        return s_current_adc_sample.err;
    }
    if (s_adc_conversion_in_group == 1U) {
        ++s_adc_discard_calls;
        /* Deliberately unrelated first conversion: real broker must drop it. */
        *raw_value = s_current_adc_sample.raw < 2048 ? 4095 : 0;
    } else {
        assert(s_adc_conversion_in_group <= 17U);
        ++s_adc_useful_calls;
        *raw_value = s_current_adc_sample.raw;
        s_last_good_raw = *raw_value;
    }
    return ESP_OK;
}


int64_t esp_timer_get_time(void)
{
    return s_now_us;
}


esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *timer)
{
    assert(args != NULL && args->callback != NULL);
    assert(timer != NULL && !s_button_timer.created);
    assert(strcmp(args->name, "button_timer") == 0);
    s_button_timer = (struct pipeline_timer){.args = *args, .created = true};
    *timer = &s_button_timer;
    return ESP_OK;
}

esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us)
{
    assert(timer == &s_button_timer && timer->created);
    assert(period_us == PIPELINE_BUTTON_PERIOD_US);
    timer->running = true;
    return ESP_OK;
}

esp_err_t esp_timer_stop(esp_timer_handle_t timer)
{
    assert(timer == &s_button_timer && timer->created);
    timer->running = false;
    return ESP_OK;
}

esp_err_t esp_timer_delete(esp_timer_handle_t timer)
{
    assert(timer == &s_button_timer && timer->created && !timer->running);
    timer->created = false;
    return ESP_OK;
}

esp_err_t gpio_intr_disable(gpio_num_t gpio)
{
    (void)gpio;
    assert(false); /* keypad drivers do not enable GPIO power-save mode */
    return ESP_ERR_INVALID_STATE;
}

static void pipeline_button_tick(void)
{
    s_now_us += PIPELINE_BUTTON_PERIOD_US;
    assert(s_button_timer.created && s_button_timer.running);
    /* Run the callback captured from production iot_button.c. It owns both
     * the real button list order and the debounce/press/release state machine. */
    s_button_timer.args.callback(s_button_timer.args.arg);
    if (s_cancel_at_adc_index > 0U && s_adc_script_index >= s_cancel_at_adc_index) {
        s_power_notification_pending = true;
        s_cancel_at_adc_index = 0U;
    }
    if (s_pipeline_tick_hook != NULL) {
        s_pipeline_tick_hook();
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
    s_queue_send_calls = 0U;
    return &s_queue;
}


BaseType_t xQueueReset(QueueHandle_t queue)
{
    assert(queue == &s_queue && s_queue_created);
    queue->head = 0U;
    queue->count = 0U;
    s_queue_send_calls = 0U;
    return pdTRUE;
}


BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait_ticks)
{
    (void)wait_ticks;
    assert(queue == &s_queue && s_queue_created);
    assert(item != NULL);
    ++s_queue_send_calls;
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

    for (unsigned tick = 0U; tick < PIPELINE_QUEUE_IDLE_TICK_LIMIT; ++tick) {
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
    if (
        reason != NULL &&
        strcmp(reason, ui_tr(UI_I18N_KEY_BOOT_CONTINUE_NO_STORAGE)) == 0
    ) {
        ++s_continue_without_storage_count;
    } else {
        ++s_recovery_prompt_count;
    }
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
        const sd_card_mount_stage_t stage =
            pipeline_mount_stage_for_result(s_mount_result);
        *report = (sd_card_mount_report_t){
            .stage = stage,
            .err = s_mount_result,
            .fresult = stage == SD_CARD_MOUNT_STAGE_FS_MOUNT
                ? s_mount_fresult
                : -1,
            .health = s_mount_result == ESP_OK
                ? SD_CARD_HEALTH_READY
                : (stage == SD_CARD_MOUNT_STAGE_FS_MOUNT
                    ? SD_CARD_HEALTH_FILESYSTEM_UNAVAILABLE
                    : SD_CARD_HEALTH_CARD_UNAVAILABLE),
        };
    }
    return s_mount_result;
}


sdmmc_card_t *sd_card_get_card_handle(void)
{
    return &s_fake_card;
}


esp_err_t sd_card_get_info(sd_card_info_t *info)
{
    assert(info != NULL);
    memset(info, 0, sizeof(*info));
    info->capacity_bytes = s_card_capacity_bytes;
    return ESP_OK;
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
    if (s_fail_next_format_start) {
        s_fail_next_format_start = false;
        ++s_format_start_errors;
        return ESP_ERR_NO_MEM;
    }
    assert(s_format_start_calls < s_format_failure_count);
    ++s_format_start_calls;
    s_current_format_status_call = 0U;
    return ESP_OK;
}


esp_err_t sd_card_format_fat32_task_get_status(sd_card_format_status_t *out_status)
{
    assert(out_status != NULL);
    assert(s_format_start_calls > 0U);
    if (s_fail_next_format_status) {
        s_fail_next_format_status = false;
        ++s_format_status_errors;
        return ESP_FAIL;
    }
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
    s_mount_result = ESP_FAIL;
    s_mount_fresult = APP_STORAGE_BOOT_FR_NO_FILESYSTEM;
    s_card_capacity_bytes = 32ULL * 1000U * 1000U * 1000U;
    s_probe_result = ESP_OK;

    /* A floating input initially looks exactly like OK. It is quarantined
     * until three consecutive high-idle samples arrive. The held OK then
     * crosses the real adapter and six-tick Button debounce despite one ADC
     * mutex timeout. */
    adc_script_append(2873, ESP_OK, 8U);
    adc_script_append(3600, ESP_OK, 3U);
    adc_script_append(3340, ESP_OK, 2U);
    adc_script_append(0, ESP_ERR_TIMEOUT, 1U);
    adc_script_append(3340, ESP_OK, 10U);
    adc_script_append(3600, ESP_OK, 80U);
    prepend_powerup_samples(2873);

    const unsigned presses_before = s_registered_press_count;
    app_storage_boot_run_diagnostic();

    assert(s_mount_calls > 0U);
    assert(s_probe_calls == 0U);
    assert(s_diagnostic_calls == 1U);
    assert(s_format_start_calls == 1U);
    assert(s_format_status_calls == 2U);
    assert(s_format_result_count == 1U);
    assert(s_format_results[0]);
    assert(s_format_result_formatted[0]);
    assert(strstr(s_last_technical_code, "fr=13") != NULL);
    assert(s_registered_press_count == presses_before + 1U);
    assert(s_registered_keys[presses_before] == KEYPAD_KEY_OK);
    assert(s_keypad_press_log_count >= 1U);
    /* The 3600, 3340 and 3600 runs each settle after three samples. */
    assert(s_keypad_transition_log_count == 3U);
    assert(strstr(s_last_keypad_transition, "raw=") != NULL);
    assert(strstr(s_last_keypad_transition, "decoded=") != NULL);
    assert(strstr(s_last_keypad_transition, "release_armed=") != NULL);
    assert(s_keypad_boot_diag_log_count == 1U); /* First monitored sample. */
    assert(s_keypad_idle_invalid_log_count == 0U); /* Gate arms before 3 s. */
    assert(s_app_keypad_events == 0U);
    assert(s_adc_read_calls > 0U && s_adc_conversion_calls > s_adc_read_calls);
    assert(s_adc_useful_calls == s_adc_discard_calls * 16U);
    assert(!s_adc_mutex_locked);
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
    assert(app_storage_boot_recover(format_eligible_mount_failure()) ==
        APP_STORAGE_BOOT_RECOVERY_SUCCEEDED);
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
    /* Two OK/release pairs settle into four transitions. The retry prompt's
     * 230-tick idle hold spans its immediate sample and one 2 s poll; the first
     * prompt adds its own initial sample (three total, two during failure). */
    assert(s_keypad_transition_log_count == 4U);
    assert(s_keypad_boot_diag_log_count == 3U);
    assert(s_failure_prompt_diag_count == 2U);
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
    s_mount_result = ESP_FAIL;
    s_mount_fresult = APP_STORAGE_BOOT_FR_NO_FILESYSTEM;
    prepend_powerup_samples(4095);
    s_card_capacity_bytes = 32ULL * 1000U * 1000U * 1000U;
    s_probe_result = ESP_OK;

    s_poweroff_jump_ready = true;
    if (setjmp(s_poweroff_jump) == 0) {
        app_storage_boot_run_diagnostic();
        assert(false);
    }
    s_poweroff_jump_ready = false;

    assert(s_mount_calls > 0U);
    assert(s_probe_calls == 0U);
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


static void prepare_independent_pipeline_case(void)
{
    prepare_keypad();
    app_storage_boot_ui_register();
    hw_inventory_init();
    adc_script_clear();
    s_mount_result = ESP_FAIL;
    s_mount_fresult = APP_STORAGE_BOOT_FR_NO_FILESYSTEM;
    s_card_capacity_bytes = 32ULL * 1000U * 1000U * 1000U;
    s_probe_result = ESP_OK;
    s_registered_press_count = 0U;
    s_keypad_press_log_count = 0U;
    s_format_result_count = 0U;
    s_format_progress_count = 0U;
    s_recovery_prompt_count = 0U;
    s_continue_without_storage_count = 0U;
    s_failure_prompt_count = 0U;
    s_boot_passed_log_count = 0U;
    s_app_keypad_events = 0U;
    s_diagnostic_calls = 0U;
}


static void test_no_card_auto_continues_without_storage(void)
{
    prepare_independent_pipeline_case();
    format_script_set((const bool[]){false}, 1U);
    s_mount_calls = 0U;
    s_probe_calls = 0U;
    s_mount_result = ESP_ERR_TIMEOUT;
    s_probe_result = ESP_OK;
    /* Align scripted ADC samples with power-up and retry delays before auto-continue. */
    prepend_powerup_samples(4095);

    const unsigned shutdowns_before = s_power_shutdown_calls;
    app_storage_boot_run_diagnostic();

    assert(s_mount_calls == 3U);
    assert(s_probe_calls == 0U);
    assert(s_format_start_calls == 0U);
    assert(s_registered_press_count == 0U);
    assert(s_power_shutdown_calls == shutdowns_before);
    assert(s_recovery_prompt_count == 0U);
    assert(s_continue_without_storage_count == 1U);
    assert(strcmp(s_last_reason, ui_tr(UI_I18N_KEY_BOOT_CONTINUE_NO_STORAGE)) == 0);
    assert(s_boot_passed_log_count == 0U);
    assert(s_diagnostic_calls == 0U);
    hw_module_info_t inventory;
    assert(hw_inventory_get(HW_MODULE_SD_CARD, &inventory));
    assert(inventory.state == HW_STATE_ABSENT);
    assert(strcmp(inventory.reason, "no response") == 0);
}


static void assert_successful_boot_after_one_ok(void)
{
    assert(s_registered_press_count == 1U);
    assert(s_registered_keys[0] == KEYPAD_KEY_OK);
    assert(s_keypad_press_log_count == 1U);
    assert(s_format_start_calls == 1U);
    assert(s_format_result_count == 1U && s_format_results[0]);
    assert(s_format_result_formatted[0]);
    assert(s_format_progress_count >= 3U);
    assert(s_format_progress[0] == 0U);
    bool saw_intermediate_progress = false;
    for (unsigned i = 0U; i < s_format_progress_count; ++i) {
        saw_intermediate_progress |= s_format_progress[i] > 0U &&
            s_format_progress[i] < 100U;
        if (i > 0U) {
            assert(s_format_progress[i] >= s_format_progress[i - 1U]);
        }
    }
    assert(saw_intermediate_progress);
    assert(s_format_progress[s_format_progress_count - 1U] == 100U);
    assert(s_diagnostic_calls == 1U);
    assert(s_boot_passed_log_count == 1U);
    assert(s_app_keypad_events == 0U);
}


static void test_ok_boundary_matrix_completes_startup(void)
{
    const int idle_values[] = {3600, 3799, 4095};
    const int ok_values[] = {2571, 2873, 3324, 3340, 3484};
    for (unsigned idle = 0U; idle < 3U; ++idle) {
        for (unsigned ok = 0U; ok < 5U; ++ok) {
            prepare_independent_pipeline_case();
            format_script_set((const bool[]){false}, 1U);
            adc_script_append(idle_values[idle], ESP_OK, 3U);
            adc_script_append(ok_values[ok], ESP_OK, 12U);
            adc_script_append(idle_values[idle], ESP_OK, 80U);
            prepend_powerup_samples(idle_values[idle]);
            app_storage_boot_run_diagnostic();
            assert_successful_boot_after_one_ok();
        }
    }
}


static void test_bounce_and_held_ok_do_not_auto_retry_failed_format(void)
{
    prepare_independent_pipeline_case();
    format_script_set((const bool[]){true, false}, 2U);
    /* A floating/low input and a high-idle spike do not arm a format. */
    adc_script_append(0, ESP_OK, 10U);
    adc_script_append(255, ESP_OK, 10U);
    adc_script_append(3600, ESP_OK, 1U);
    adc_script_append(3340, ESP_OK, 10U);
    adc_script_append(3600, ESP_OK, 3U);
    /* A 20 ms excursion is shorter than the real vendor debounce. */
    adc_script_append(3340, ESP_OK, 2U);
    adc_script_append(3600, ESP_OK, 12U);
    /* One long hold must not be replayed when the failed-format prompt opens. */
    adc_script_append(3340, ESP_OK, 300U);
    adc_script_append(3600, ESP_OK, 30U);
    adc_script_append(3340, ESP_OK, 12U);
    adc_script_append(3600, ESP_OK, 80U);
    prepend_powerup_samples(0);
    app_storage_boot_run_diagnostic();
    assert(s_registered_press_count == 2U);
    assert(s_registered_keys[0] == KEYPAD_KEY_OK);
    assert(s_registered_keys[1] == KEYPAD_KEY_OK);
    assert(s_format_start_calls == 2U);
    assert(s_format_result_count == 2U);
    assert(!s_format_results[0] && s_format_results[1]);
    assert(s_failure_prompt_count == 1U);
    assert(s_boot_passed_log_count == 1U);
}


static void test_format_start_or_status_error_allows_ok_retry(void)
{
    for (unsigned failure = 0U; failure < 2U; ++failure) {
        prepare_independent_pipeline_case();
        format_script_set((const bool[]){false, false}, 2U);
        s_fail_next_format_start = failure == 0U;
        s_fail_next_format_status = failure == 1U;
        adc_script_append(3600, ESP_OK, 3U);
        adc_script_append(3340, ESP_OK, 12U);
        adc_script_append(3600, ESP_OK, 80U);
        adc_script_append(3340, ESP_OK, 12U);
        adc_script_append(3600, ESP_OK, 80U);
        prepend_powerup_samples(3600);
        app_storage_boot_run_diagnostic();
        assert(s_registered_press_count == 2U);
        assert(s_format_result_count == 2U);
        assert(!s_format_results[0] && s_format_results[1]);
        assert(s_format_start_errors == (failure == 0U ? 1U : 0U));
        assert(s_format_status_errors == (failure == 1U ? 1U : 0U));
        assert(s_failure_prompt_count == 1U);
        assert(s_boot_passed_log_count == 1U);
        assert(strcmp(s_last_primary_hint, ui_tr(UI_I18N_KEY_BOOT_FORMAT_RETRY_HINT)) == 0);
        assert(strcmp(s_last_retry_hint, ui_tr(UI_I18N_KEY_BOOT_FORMAT_POWER_HINT)) == 0);
    }
}


static void test_recorded_low_adc_observations_do_not_register_or_format(void)
{
    /* Each five-tick constant observation can settle; sub-100 ramps and the
     * LEFT-threshold chatter cannot. The zero/119/zero/112/zero jumps settle. */
    const int observed_raw[] = {
        104, 124, 137, 155, 0, 15, 43, 70, 89, 119, 114, 117,
        115, 116, 115, 117, 110, 117, 114, 117, 115, 122, 115, 121,
        142, 159, 179, 0, 9, 40, 64, 91, 112, 117, 109, 116,
        115, 116, 112, 117, 114, 116, 115, 119, 112, 120, 113, 116,
        115, 117, 115, 116, 114, 121, 113, 119, 115, 122, 142, 153,
        172, 0, 14, 0, 25, 1, 0, 12, 40, 69,
    };
    prepare_independent_pipeline_case();
    format_script_set((const bool[]){false}, 1U);
    for (size_t i = 0U; i < sizeof(observed_raw) / sizeof(observed_raw[0]); ++i) {
        adc_script_append(observed_raw[i], ESP_OK, 5U);
    }
    /* Exit via the existing power notification after consuming observations. */
    s_cancel_at_adc_index = s_adc_script_count;
    assert(app_storage_boot_recover(write_probe_failure()) ==
        APP_STORAGE_BOOT_RECOVERY_ABORTED);
    assert(s_adc_script_index == s_adc_script_count);
    assert(s_registered_press_count == 0U);
    assert(s_keypad_press_log_count == 0U);
    assert(s_keypad_transition_log_count == 5U);
    assert(s_keypad_boot_diag_log_count == 1U); /* Immediate monitored sample. */
    assert(s_keypad_idle_invalid_log_count == 1U); /* First 3 s deadline only. */
    assert(s_format_start_calls == 0U);
    assert(s_format_status_calls == 0U);
    assert(s_format_result_count == 0U);
    assert(s_boot_passed_log_count == 0U);
    assert(s_app_keypad_events == 0U);
    assert(s_adc_useful_calls == s_adc_discard_calls * 16U);
    assert(!s_adc_mutex_locked);
}


static void test_probe_failure_ok_key_stays_on_non_destructive_retry_screen(void)
{
    prepare_independent_pipeline_case();
    format_script_set((const bool[]){false}, 1U);
    s_recovery_prompt_count = 0U;
    s_mount_calls = 0U;
    s_probe_calls = 0U;
    s_mount_result = ESP_OK;
    s_probe_result = ESP_OK;
    adc_script_append(4000, ESP_OK, 3U);
    adc_script_append(3340, ESP_OK, 12U); /* OK is not a format decision here. */
    adc_script_append(4000, ESP_OK, 20U);
    adc_script_append(1800, ESP_OK, 12U); /* RIGHT retries the mounted card. */
    adc_script_append(4000, ESP_OK, 80U);

    assert(app_storage_boot_recover(write_probe_failure()) ==
        APP_STORAGE_BOOT_RECOVERY_SUCCEEDED);

    assert(s_recovery_prompt_count == 1U);
    assert(strcmp(s_last_reason, ui_tr(UI_I18N_KEY_BOOT_REASON_FS_MOUNT)) == 0);
    assert(strcmp(
        s_last_technical_code,
        "WRITE_PROBE/READ err=ESP_FAIL errno=2"
    ) == 0);
    assert(s_last_warning[0] == '\0');
    assert(strcmp(
        s_last_primary_hint,
        ui_tr(UI_I18N_KEY_BOOT_RECOVERY_RETRY_HINT)
    ) == 0);
    assert(strcmp(s_last_retry_hint, ui_tr(UI_I18N_KEY_BOOT_ERROR_FOOTER)) == 0);
    assert(strcmp(
        s_last_cancel_hint,
        ui_tr(UI_I18N_KEY_BOOT_RECOVERY_CANCEL_HINT)
    ) == 0);
    assert(s_registered_press_count == 2U);
    assert(s_registered_keys[0] == KEYPAD_KEY_OK);
    assert(s_registered_keys[1] == KEYPAD_KEY_RIGHT);
    assert(s_mount_calls == 1U && s_probe_calls == 1U);
    assert(s_format_start_calls == 0U);
    assert(s_format_status_calls == 0U);
}


typedef struct {
    int time_ms;
    int raw;
    bool abrupt_press;
} field_replay_landmark_t;


static const field_replay_landmark_t s_field_replay_landmarks[] = {
    {1621, 104, false}, {1881, 124, false}, {2631, 137, false},
    {4641, 155, false}, {5861, 0, true},   {7861, 15, false},
    {9861, 43, false},  {11861, 70, false}, {13861, 89, false},
    {15861, 119, false}, {18451, 142, false}, {20451, 159, false},
    {22451, 179, false}, {22681, 0, true}, {24681, 9, false},
    {26681, 40, false}, {28681, 64, false}, {30681, 91, false},
    {32681, 112, false}, {35841, 142, false}, {37841, 153, false},
    {39841, 172, false}, {40491, 0, true}, {42491, 14, false},
    {44491, 0, false}, {46491, 25, false}, {48491, 1, false},
    {50491, 0, false}, {52491, 12, false}, {54491, 40, false},
    {56491, 69, false},
};


static int field_replay_raw_at(int time_ms)
{
    const size_t landmark_count = sizeof(s_field_replay_landmarks) /
        sizeof(s_field_replay_landmarks[0]);
    if (time_ms <= s_field_replay_landmarks[0].time_ms) {
        return s_field_replay_landmarks[0].raw;
    }

    size_t right = 1U;
    while (
        right < landmark_count &&
        time_ms > s_field_replay_landmarks[right].time_ms
    ) {
        ++right;
    }
    if (right == landmark_count) {
        return s_field_replay_landmarks[landmark_count - 1U].raw;
    }

    const field_replay_landmark_t *left_landmark =
        &s_field_replay_landmarks[right - 1U];
    const field_replay_landmark_t *right_landmark =
        &s_field_replay_landmarks[right];
    if (time_ms == right_landmark->time_ms) {
        return right_landmark->raw;
    }
    if (right_landmark->abrupt_press) {
        return left_landmark->raw;
    }

    const int time_delta = time_ms - left_landmark->time_ms;
    const int interval = right_landmark->time_ms - left_landmark->time_ms;
    const int raw_delta = right_landmark->raw - left_landmark->raw;
    return left_landmark->raw + raw_delta * time_delta / interval;
}


static int field_replay_sample_raw(size_t sample_index)
{
    static const int chatter_offsets[] = {
        5, -5, 3, -4, 4, -3, 2, -5, 5, -2, 1, -4,
    };
    const int time_ms = 1621 + (int)(sample_index * 10U);
    int raw = field_replay_raw_at(time_ms);
    int window_start_ms = -1;
    unsigned chatter_period_ms = 0U;
    if (time_ms >= 15861 && time_ms <= 16451) {
        window_start_ms = 15861;
        chatter_period_ms = 39U;
    } else if (time_ms >= 32691 && time_ms <= 33841) {
        window_start_ms = 32691;
        chatter_period_ms = 46U;
    }
    if (window_start_ms >= 0) {
        /* Hold each table value for 39/46 ms to match the observed flip rate. */
        const size_t offset_index = (size_t)(time_ms - window_start_ms) /
            chatter_period_ms;
        raw = 115 + chatter_offsets[
            offset_index % (sizeof(chatter_offsets) / sizeof(chatter_offsets[0]))
        ];
    }
    return raw;
}


static unsigned pipeline_abs_diff(int left, int right)
{
    return left >= right
        ? (unsigned)(left - right)
        : (unsigned)(right - left);
}


static void field_replay_snapshot_hook(void)
{
    if (
        !s_field_replay_snapshot_pending ||
        s_adc_script_index != FIELD_REPLAY_TICK_COUNT
    ) {
        return;
    }
    s_field_replay_snapshot_pending = false;
    s_pipeline_tick_hook = NULL;
    s_field_replay_transitions = s_keypad_transition_log_count;
    s_field_replay_boot_input = s_keypad_boot_diag_log_count;
    s_field_replay_idle_invalid = s_keypad_idle_invalid_log_count;
    s_field_replay_presses = s_keypad_press_log_count;
    s_field_replay_snapshot_record_end = s_keypad_diag_record_count;
    printf(
        "KS_REPLAY_STATS transitions=%u boot_input=%u idle_invalid=%u max_raw=%d presses=%u\n",
        s_field_replay_transitions,
        s_field_replay_boot_input,
        s_field_replay_idle_invalid,
        s_field_replay_max_raw,
        s_field_replay_presses
    );
    (void)fflush(stdout);

    /* This is first so the legacy chatter policy fails at its decisive bound. */
    assert(s_field_replay_transitions <= 8U);
    assert(s_field_replay_presses == 0U);
    assert(s_registered_press_count == 0U);
    assert(s_queue_send_calls == 0U && s_queue.count == 0U);
    assert(s_format_start_calls == 0U);
    assert(s_format_status_calls == 0U && s_format_result_count == 0U);
    assert(s_app_keypad_events == 0U);
    assert(s_field_replay_max_raw <= 180);

    int previous_logged_raw = 104;
    int64_t last_input_or_transition_us = -1LL;
    int64_t first_idle_warning_us = -1LL;
    int64_t previous_idle_warning_us = -1LL;
    unsigned idle_warning_count = 0U;
    for (size_t i = 0U; i < s_field_replay_snapshot_record_end; ++i) {
        const pipeline_diag_record_t *record = &s_keypad_diag_records[i];
        if (record->kind == PIPELINE_DIAG_TRANSITION) {
            last_input_or_transition_us = record->time_us;
        } else if (record->kind == PIPELINE_DIAG_BOOT_INPUT) {
            if (last_input_or_transition_us >= 0LL) {
                assert(record->time_us - last_input_or_transition_us >= 1990000LL);
            }
            last_input_or_transition_us = record->time_us;
        }
        if (record->kind == PIPELINE_DIAG_TRANSITION) {
            assert(record->raw_valid);
            assert(pipeline_abs_diff(record->raw, previous_logged_raw) >= 100U);
            assert(strcmp(
                record->from,
                keypad_key_name(keypad_decode_raw(previous_logged_raw))
            ) == 0);
            assert(strcmp(
                record->to,
                keypad_key_name(keypad_decode_raw(record->raw))
            ) == 0);
            assert(strcmp(
                record->decoded,
                keypad_key_name(keypad_decode_raw(record->raw))
            ) == 0);
            previous_logged_raw = record->raw;
        } else if (record->kind == PIPELINE_DIAG_BOOT_INPUT) {
            assert(record->raw_valid);
        } else if (record->kind == PIPELINE_DIAG_IDLE_INVALID) {
            char expected[256];
            assert(record->raw_valid);
            assert(record->needed_raw == 3600);
            (void)snprintf(
                expected,
                sizeof(expected),
                "KEYPAD_IDLE_LEVEL_INVALID raw=%d need>=3600: key line is not pulled high - check keypad module + (3V3), - (GND) and S (GPIO8) wiring; no key can be detected",
                record->raw
            );
            assert(strcmp(record->line, expected) == 0);
            if (first_idle_warning_us < 0LL) {
                first_idle_warning_us = record->time_us;
            }
            if (previous_idle_warning_us >= 0LL) {
                assert(record->time_us - previous_idle_warning_us >= 5000000LL);
            }
            previous_idle_warning_us = record->time_us;
            ++idle_warning_count;
        }
    }
    assert(s_field_replay_boot_input <= 29U);
    assert(s_field_replay_idle_invalid >= 1U);
    assert(s_field_replay_idle_invalid <= 12U);
    assert(idle_warning_count == s_field_replay_idle_invalid);
    assert(first_idle_warning_us >= 1611000LL + 3000000LL);
}


static void test_field_replay_then_wiring_recovery_uses_same_session(void)
{
    prepare_independent_pipeline_case();
    format_script_set((const bool[]){false}, 1U);
    s_mount_result = ESP_OK;
    s_probe_result = ESP_OK;
    s_storage_init_calls = 0U;
    s_recovery_prompt_count = 0U;
    s_now_us = 1611000LL;
    s_field_replay_max_raw = 0;

    for (size_t i = 0U; i < FIELD_REPLAY_TICK_COUNT; ++i) {
        const int raw = field_replay_sample_raw(i);
        if (raw > s_field_replay_max_raw) {
            s_field_replay_max_raw = raw;
        }
        adc_script_append(raw, ESP_OK, 1U);
    }
    for (unsigned sample = 0U; sample < 220U; ++sample) {
        adc_script_append(4000, ESP_OK, 1U);
    }
    adc_script_append(1800, ESP_OK, 12U);
    adc_script_append(4000, ESP_OK, 80U);

    s_field_replay_snapshot_pending = true;
    s_pipeline_tick_hook = field_replay_snapshot_hook;
    app_storage_boot_recover_storage_manager_failure(ESP_FAIL);
    s_pipeline_tick_hook = NULL;

    assert(!s_field_replay_snapshot_pending);
    assert(s_registered_press_count == 1U);
    assert(s_registered_keys[0] == KEYPAD_KEY_RIGHT);
    assert(s_keypad_press_log_count == 1U);
    assert(s_app_keypad_events == 0U);
    assert(s_storage_init_calls == 1U);
    assert(s_format_start_calls == 0U);
    assert(s_format_status_calls == 0U);
    assert(s_format_result_count == 1U && s_format_results[0]);
    assert(!s_format_result_formatted[0]);
    assert(s_format_progress_count == 0U);
    assert(s_recovery_prompt_count == 1U);
    assert(s_display_init_calls > 0U && s_boot_ui_init_calls > 0U);
    assert(s_adc_useful_calls == s_adc_discard_calls * 16U);
    assert(!s_adc_mutex_locked);

    int64_t gate_armed_transition_us = -1LL;
    bool saw_armed_periodic_after_idle = false;
    for (
        size_t i = s_field_replay_snapshot_record_end;
        i < s_keypad_diag_record_count;
        ++i
    ) {
        const pipeline_diag_record_t *record = &s_keypad_diag_records[i];
        if (record->kind == PIPELINE_DIAG_IDLE_INVALID) {
            assert(false); /* Once armed, a later press must not restart warnings. */
        }
        if (
            record->kind == PIPELINE_DIAG_TRANSITION &&
            record->raw == 4000 &&
            record->release_armed == 1U
        ) {
            gate_armed_transition_us = record->time_us;
        }
        if (
            record->kind == PIPELINE_DIAG_BOOT_INPUT &&
            record->raw_valid &&
            record->release_armed == 1U &&
            gate_armed_transition_us >= 0LL &&
            record->time_us > gate_armed_transition_us
        ) {
            saw_armed_periodic_after_idle = true;
        }
    }
    assert(gate_armed_transition_us >= 0LL);
    assert(saw_armed_periodic_after_idle);

    s_probe_result = ESP_OK;
    prepend_powerup_samples(4000);
    app_storage_boot_run_diagnostic();
    assert(s_mount_calls > 0U && s_probe_calls > 0U);
    assert(s_diagnostic_calls == 1U);
    assert(s_boot_passed_log_count == 1U);
    assert(s_format_start_calls == 0U);
}


static void test_field_replay_prints_stats_for_visibility_fail_before(void)
{
    prepare_independent_pipeline_case();
    s_now_us = 1611000LL;
    s_field_replay_max_raw = 0;
    for (size_t i = 0U; i < FIELD_REPLAY_TICK_COUNT; ++i) {
        const int raw = field_replay_sample_raw(i);
        if (raw > s_field_replay_max_raw) {
            s_field_replay_max_raw = raw;
        }
        adc_script_append(raw, ESP_OK, 1U);
    }
    s_cancel_at_adc_index = s_adc_script_count;
    assert(app_storage_boot_recover(format_eligible_mount_failure()) ==
        APP_STORAGE_BOOT_RECOVERY_ABORTED);
    assert(s_adc_script_index == FIELD_REPLAY_TICK_COUNT);
    printf(
        "KS_REPLAY_STATS transitions=%u boot_input=%u idle_invalid=%u max_raw=%d presses=%u\n",
        s_keypad_transition_log_count,
        s_keypad_boot_diag_log_count,
        s_keypad_idle_invalid_log_count,
        s_field_replay_max_raw,
        s_keypad_press_log_count
    );
    (void)fflush(stdout);
    prepare_keypad();
    adc_script_clear();
}


static unsigned s_visibility_callback_calls;


static void visibility_callback(keypad_key_t key, void *context)
{
    (void)key;
    (void)context;
    ++s_visibility_callback_calls;
}


static pipeline_diag_record_t *pipeline_transition_record(unsigned ordinal)
{
    unsigned seen = 0U;
    for (size_t i = 0U; i < s_keypad_diag_record_count; ++i) {
        if (s_keypad_diag_records[i].kind == PIPELINE_DIAG_TRANSITION) {
            if (seen++ == ordinal) {
                return &s_keypad_diag_records[i];
            }
        }
    }
    return NULL;
}


static int visibility_jitter_offset(unsigned index)
{
    static const int offsets[] = {-8, 8, -5, 5, -3, 3, -7, 7, -1, 1};
    return offsets[index % (sizeof(offsets) / sizeof(offsets[0]))];
}


static void test_keypad_settled_level_visibility_matrix(void)
{
    static const int levels[] = {600, 2000, 3900, 300, 900, 1600, 2400};
    s_visibility_callback_calls = 0U;
    for (size_t level_index = 0U;
         level_index < sizeof(levels) / sizeof(levels[0]);
         ++level_index) {
        const int level = levels[level_index];
        prepare_keypad();
        adc_script_clear();
        s_registered_press_count = 0U;
        s_app_keypad_events = 0U;
        assert(keypad_start_boot_event_monitor(visibility_callback, NULL) == ESP_OK);

        for (unsigned sample = 0U; sample < 20U; ++sample) {
            adc_script_append(150, ESP_OK, 1U);
        }
        for (unsigned sample = 0U; sample < 100U; ++sample) {
            adc_script_append(150 + visibility_jitter_offset(sample), ESP_OK, 1U);
        }
        for (unsigned sample = 0U; sample < 20U; ++sample) {
            adc_script_append(level, ESP_OK, 1U);
        }
        for (unsigned sample = 0U; sample < 100U; ++sample) {
            adc_script_append(level + visibility_jitter_offset(sample), ESP_OK, 1U);
        }
        for (unsigned sample = 0U; sample < 20U; ++sample) {
            adc_script_append(150, ESP_OK, 1U);
        }

        for (unsigned sample = 0U; sample < 120U; ++sample) {
            pipeline_button_tick();
        }
        assert(s_keypad_transition_log_count == 0U);
        pipeline_button_tick();
        pipeline_button_tick();
        assert(s_keypad_transition_log_count == 0U);
        pipeline_button_tick();
        assert(s_keypad_transition_log_count == 1U);
        for (unsigned sample = 3U; sample < 20U; ++sample) {
            pipeline_button_tick();
        }
        for (unsigned sample = 0U; sample < 100U; ++sample) {
            pipeline_button_tick();
        }
        assert(s_keypad_transition_log_count == 1U);
        for (unsigned sample = 0U; sample < 2U; ++sample) {
            pipeline_button_tick();
        }
        assert(s_keypad_transition_log_count == 1U);
        pipeline_button_tick();
        assert(s_keypad_transition_log_count == 2U);
        for (unsigned sample = 3U; sample < 20U; ++sample) {
            pipeline_button_tick();
        }

        pipeline_diag_record_t *to_level = pipeline_transition_record(0U);
        pipeline_diag_record_t *back_to_idle = pipeline_transition_record(1U);
        assert(to_level != NULL && back_to_idle != NULL);
        const char *level_name = keypad_key_name(keypad_decode_raw(level));
        assert(to_level->raw == level);
        assert(strcmp(to_level->from, "NONE") == 0);
        assert(strcmp(to_level->to, level_name) == 0);
        assert(strcmp(to_level->decoded, level_name) == 0);
        /* At 3900 the third sample also satisfies the 3-sample release gate. */
        assert(to_level->release_armed == (level == 3900 ? 1U : 0U));
        assert(back_to_idle->raw == 150);
        assert(strcmp(back_to_idle->to, "NONE") == 0);
        assert(s_registered_press_count == 0U);
        assert(s_visibility_callback_calls == 0U);
        assert(s_app_keypad_events == 0U);
    }

    prepare_keypad();
    adc_script_clear();
    s_visibility_callback_calls = 0U;
    assert(keypad_start_boot_event_monitor(visibility_callback, NULL) == ESP_OK);
    for (unsigned sample = 0U; sample < 20U; ++sample) {
        adc_script_append(150, ESP_OK, 1U);
    }
    for (unsigned sample = 0U; sample < 20U; ++sample) {
        adc_script_append(0, ESP_OK, 1U);
    }
    for (unsigned sample = 0U; sample < 20U; ++sample) {
        pipeline_button_tick();
    }
    assert(s_keypad_transition_log_count == 0U);
    pipeline_button_tick();
    pipeline_button_tick();
    assert(s_keypad_transition_log_count == 0U);
    pipeline_button_tick();
    assert(s_keypad_transition_log_count == 1U);
    pipeline_diag_record_t *press_to_ground = pipeline_transition_record(0U);
    assert(press_to_ground != NULL);
    assert(press_to_ground->raw == 0);
    assert(strcmp(press_to_ground->from, "NONE") == 0);
    assert(strcmp(press_to_ground->to, "LEFT") == 0);
    assert(strcmp(press_to_ground->decoded, "LEFT") == 0);
    assert(press_to_ground->release_armed == 0U);
    assert(s_visibility_callback_calls == 0U);
    keypad_stop_event_monitor();
}


int main(void)
{
    const char *scenario = getenv("KS_PIPELINE_SCENARIO");
    s_saved_locale = UI_LOCALE_UK;
    app_storage_boot_ui_register();

    if (scenario != NULL && strcmp(scenario, "A") == 0) {
        test_field_replay_then_wiring_recovery_uses_same_session();
        keypad_stop_event_monitor();
        return 0;
    }
    if (scenario != NULL && strcmp(scenario, "C") == 0) {
        test_field_replay_prints_stats_for_visibility_fail_before();
        test_keypad_settled_level_visibility_matrix();
        keypad_stop_event_monitor();
        return 0;
    }

    test_first_press_after_3600_idle_completes_pipeline();
    test_failed_format_shows_retry_then_second_ok_succeeds();
    test_failed_format_power_notification_cancels_and_powers_off();
    test_no_card_auto_continues_without_storage();
    test_ok_boundary_matrix_completes_startup();
    test_bounce_and_held_ok_do_not_auto_retry_failed_format();
    test_format_start_or_status_error_allows_ok_retry();
    test_recorded_low_adc_observations_do_not_register_or_format();
    test_probe_failure_ok_key_stays_on_non_destructive_retry_screen();
    test_field_replay_then_wiring_recovery_uses_same_session();
    test_keypad_settled_level_visibility_matrix();

    keypad_stop_event_monitor();
    puts("APP_STORAGE_BOOT_KEYPAD_PIPELINE_PASS");
    return 0;
}
