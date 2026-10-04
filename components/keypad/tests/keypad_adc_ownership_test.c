#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "adc_service.h"
#include "app_events.h"
#include "board_config.h"
#include "button_interface.h"
#include "button_types.h"
#include "esp_timer.h"
#include "iot_button.h"
#include "keypad.h"


struct test_button {
    const button_driver_t *driver;
};


static const button_driver_t *s_drivers[5];
static struct test_button s_buttons[5];
static button_cb_t s_callbacks[5];
static void *s_callback_user_data[5];
static size_t s_created;
static int s_service_reads;
static size_t s_last_sample_count;
static int s_raw_value;
static int64_t s_now_us;
static int64_t s_read_duration_us;
static bool s_service_busy;
static esp_err_t s_service_error;
static int s_try_reads;
static int s_direct_adc_reads;
static int s_keypad_events_posted;
static int32_t s_last_keypad_event_value;
static unsigned s_boot_callback_calls;
static keypad_key_t s_boot_callback_key;
static void *s_boot_callback_expected_context;
static unsigned s_keypad_log_count;
static char s_last_keypad_log[512];
static unsigned s_keypad_transition_log_count;
static unsigned s_keypad_boot_diag_log_count;
static unsigned s_keypad_idle_warning_log_count;
static char s_last_transition_log[512];
static char s_last_idle_warning_log[512];
static int64_t s_transition_log_times[256];

void keypad_test_log_capture(const char *tag, const char *format, va_list args)
{
    (void)tag;
    if (strncmp(format, "KEYPAD_BUTTON_PRESS", 19U) == 0) {
        ++s_keypad_log_count;
        (void)vsnprintf(s_last_keypad_log, sizeof(s_last_keypad_log), format, args);
    } else if (strncmp(format, "KEYPAD_RAW_TRANSITION", 21U) == 0) {
        ++s_keypad_transition_log_count;
        if (s_keypad_transition_log_count <=
            sizeof(s_transition_log_times) / sizeof(s_transition_log_times[0])) {
            s_transition_log_times[s_keypad_transition_log_count - 1U] = s_now_us;
        }
        (void)vsnprintf(
            s_last_transition_log,
            sizeof(s_last_transition_log),
            format,
            args
        );
    } else if (strncmp(format, "KEYPAD_BOOT_INPUT", 17U) == 0) {
        ++s_keypad_boot_diag_log_count;
    } else if (strncmp(format, "KEYPAD_IDLE_LEVEL_INVALID", 25U) == 0) {
        ++s_keypad_idle_warning_log_count;
        (void)vsnprintf(
            s_last_idle_warning_log,
            sizeof(s_last_idle_warning_log),
            format,
            args
        );
    }
}


esp_err_t keypad_button_adapter_start_boot(
    keypad_event_callback_t callback,
    void *user_context
);
void keypad_button_adapter_stop_boot(void);


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


esp_err_t adc_service_read_average_raw(
    adc_channel_t channel,
    size_t sample_count,
    int *raw_value
)
{
    assert(channel == 7);
    assert(raw_value != NULL);
    s_service_reads++;
    s_last_sample_count = sample_count;
    *raw_value = s_raw_value;
    s_now_us += s_read_duration_us;
    return ESP_OK;
}


esp_err_t adc_service_try_read_average_raw(
    adc_channel_t channel,
    size_t sample_count,
    int *raw_value
)
{
    s_try_reads++;
    if (s_service_busy) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_service_error != ESP_OK) {
        return s_service_error;
    }

    return adc_service_read_average_raw(
        channel,
        sample_count,
        raw_value
    );
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
    assert(driver != NULL);
    assert(driver->get_key_level != NULL);
    assert(driver->del != NULL);
    assert(ret_button != NULL);
    assert(s_created < sizeof(s_drivers) / sizeof(s_drivers[0]));

    s_drivers[s_created] = driver;
    s_buttons[s_created].driver = driver;
    *ret_button = &s_buttons[s_created];
    s_created++;
    return ESP_OK;
}


esp_err_t iot_button_register_cb(
    button_handle_t btn_handle,
    button_event_t event,
    button_event_args_t *event_args,
    button_cb_t cb,
    void *usr_data
)
{
    assert(btn_handle != NULL);
    assert(event == BUTTON_PRESS_DOWN);
    assert(event_args == NULL);
    assert(cb != NULL);
    assert(usr_data != NULL);

    const size_t index = (size_t)(btn_handle - s_buttons);
    assert(index < 5U);
    s_callbacks[index] = cb;
    s_callback_user_data[index] = usr_data;
    return ESP_OK;
}


esp_err_t iot_button_delete(button_handle_t btn_handle)
{
    assert(btn_handle != NULL);
    assert(btn_handle->driver->del != NULL);
    return btn_handle->driver->del((button_driver_t *)btn_handle->driver);
}


esp_err_t app_events_post(
    app_event_type_t type,
    int32_t value
)
{
    assert(type == APP_EVENT_KEYPAD);
    s_keypad_events_posted++;
    s_last_keypad_event_value = value;
    return ESP_OK;
}


esp_err_t adc_service_init(void)
{
    return ESP_OK;
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
    s_direct_adc_reads++;
    return ESP_ERR_INVALID_STATE;
}


const char *keypad_key_name(keypad_key_t key)
{
    static const char *const names[] = {
        "NONE",
        "LEFT",
        "UP",
        "DOWN",
        "RIGHT",
        "OK",
    };
    assert(key >= KEYPAD_KEY_NONE && key <= KEYPAD_KEY_OK);
    return names[key];
}


esp_err_t keypad_button_adapter_init(void);
void keypad_button_adapter_stop(void);


static void reset_test_state(void)
{
    s_created = 0;
    s_service_reads = 0;
    s_last_sample_count = 0;
    s_read_duration_us = 0;
    s_service_busy = false;
    s_service_error = ESP_OK;
    s_try_reads = 0;
    s_direct_adc_reads = 0;
    s_now_us = 10000;
    s_raw_value = 550;
    s_keypad_events_posted = 0;
    s_last_keypad_event_value = 0;
    s_boot_callback_calls = 0U;
    s_boot_callback_key = KEYPAD_KEY_NONE;
    s_boot_callback_expected_context = NULL;
    s_keypad_log_count = 0U;
    s_last_keypad_log[0] = '\0';
    s_keypad_transition_log_count = 0U;
    s_keypad_boot_diag_log_count = 0U;
    s_keypad_idle_warning_log_count = 0U;
    s_last_transition_log[0] = '\0';
    s_last_idle_warning_log[0] = '\0';
    memset(s_transition_log_times, 0, sizeof(s_transition_log_times));

    for (size_t i = 0U; i < 5U; ++i) {
        s_callbacks[i] = NULL;
        s_callback_user_data[i] = NULL;
    }
}


static void test_keypad_buttons_use_one_broker_sample(void)
{
    reset_test_state();

    assert(keypad_button_adapter_init() == ESP_OK);
    assert(s_created == 5U);

    /* A key-range value before a released baseline is quarantined. This is
     * the disconnected/floating-input case from the supplied HIL log. */
    for (size_t i = 0U; i < 5U; ++i) {
        assert(s_drivers[i]->get_key_level((button_driver_t *)s_drivers[i]) == BUTTON_INACTIVE);
    }
    assert(s_service_reads == 1);
    assert(s_last_sample_count == 16U);
    assert(s_try_reads == 1);

    s_now_us += 1001;
    s_raw_value = 4095;
    assert(s_drivers[1]->get_key_level((button_driver_t *)s_drivers[1]) == BUTTON_INACTIVE);
    s_now_us += 1001;
    assert(s_drivers[1]->get_key_level((button_driver_t *)s_drivers[1]) == BUTTON_INACTIVE);
    s_now_us += 1001;
    assert(s_drivers[1]->get_key_level((button_driver_t *)s_drivers[1]) == BUTTON_INACTIVE);

    s_now_us += 1001;
    s_raw_value = 550;

    /* A calibrated UP value must be active, and the first level query must go
     * through the service with the same 16-sample depth as the compatibility
     * read path. */
    assert(s_drivers[1]->get_key_level((button_driver_t *)s_drivers[1]) == BUTTON_ACTIVE);
    assert(s_service_reads == 5);
    assert(s_last_sample_count == 16U);
    assert(s_try_reads == 5);

    /* Every Button object in the same timer tick sees the same broker-owned
     * conversion; no second direct ADC read is allowed. */
    assert(s_drivers[0]->get_key_level((button_driver_t *)s_drivers[0]) == BUTTON_INACTIVE);
    assert(s_drivers[4]->get_key_level((button_driver_t *)s_drivers[4]) == BUTTON_INACTIVE);
    assert(s_service_reads == 5);
    assert(s_direct_adc_reads == 0);

    /* The old direct threshold decoder turned these adjacent readings into a
     * DOWN transition. The boundary deadband must retain UP and keep DOWN
     * inactive instead. */
    s_now_us += 1001;
    s_raw_value = 899;
    assert(s_drivers[1]->get_key_level((button_driver_t *)s_drivers[1]) == BUTTON_ACTIVE);
    assert(s_drivers[2]->get_key_level((button_driver_t *)s_drivers[2]) == BUTTON_INACTIVE);
    assert(s_service_reads == 6);

    s_now_us += 1001;
    s_raw_value = 906;
    assert(s_drivers[1]->get_key_level((button_driver_t *)s_drivers[1]) == BUTTON_ACTIVE);
    assert(s_drivers[2]->get_key_level((button_driver_t *)s_drivers[2]) == BUTTON_INACTIVE);
    assert(s_service_reads == 7);

    /* If the timer task finds the broker busy, preserve the last valid key
     * rather than synthesizing a release. A later logical Button callback in
     * this same timer tick must not retry the ADC read. */
    s_now_us += 1001;
    s_service_busy = true;
    assert(s_drivers[1]->get_key_level((button_driver_t *)s_drivers[1]) == BUTTON_ACTIVE);
    assert(s_drivers[2]->get_key_level((button_driver_t *)s_drivers[2]) == BUTTON_INACTIVE);
    assert(s_service_reads == 7);
    assert(s_try_reads == 8);

    /* After the failed-attempt window expires, acquire the new raw value. The
     * fake broker advances time during the read so the cache timestamp must
     * be taken after completion; the next Button object still reuses it. */
    s_now_us += 1001;
    s_service_busy = false;
    s_read_duration_us = 1500;
    s_raw_value = 1261;
    assert(s_drivers[2]->get_key_level((button_driver_t *)s_drivers[2]) == BUTTON_ACTIVE);
    assert(s_service_reads == 8);
    assert(s_try_reads == 9);
    assert(s_drivers[3]->get_key_level((button_driver_t *)s_drivers[3]) == BUTTON_INACTIVE);
    assert(s_service_reads == 8);
    assert(s_direct_adc_reads == 0);

    keypad_button_adapter_stop();
}


static void test_debounced_button_callback_posts_keypad_event(void)
{
    reset_test_state();

    assert(keypad_button_adapter_init() == ESP_OK);
    assert(s_created == 5U);

    for (size_t i = 0U; i < 5U; ++i) {
        assert(s_callbacks[i] != NULL);
        assert(s_callback_user_data[i] != NULL);

        s_callbacks[i](&s_buttons[i], s_callback_user_data[i]);
        assert(s_keypad_events_posted == (int)(i + 1U));
        assert(s_last_keypad_event_value == (int32_t)(i + 1U));
        assert(s_keypad_log_count == i + 1U);
        char expected_log[80];
        (void)snprintf(
            expected_log,
            sizeof(expected_log),
            "KEYPAD_BUTTON_PRESS key=%s value=%d",
            keypad_key_name((keypad_key_t)(i + 1U)),
            (int)(i + 1U)
        );
        assert(strcmp(s_last_keypad_log, expected_log) == 0);
    }

    keypad_button_adapter_stop();
}


static void boot_prompt_callback(keypad_key_t key, void *user_context)
{
    assert(user_context == s_boot_callback_expected_context);
    ++s_boot_callback_calls;
    s_boot_callback_key = key;
}


static void test_boot_callback_routes_only_while_prompt_is_active(void)
{
    reset_test_state();
    assert(keypad_button_adapter_init() == ESP_OK);

    int context_token = 0;
    s_boot_callback_expected_context = &context_token;
    assert(keypad_button_adapter_start_boot(
        boot_prompt_callback,
        &context_token
    ) == ESP_OK);

    s_raw_value = 3800;
    for (unsigned sample = 0U; sample < 3U; ++sample) {
        s_now_us += 1001;
        assert(s_drivers[4]->get_key_level((button_driver_t *)s_drivers[4]) == BUTTON_INACTIVE);
    }
    /* The first sample logs immediately; the >2 s poll logs the second. */
    assert(s_keypad_boot_diag_log_count == 1U);
    s_now_us += 2000001;
    assert(s_drivers[4]->get_key_level((button_driver_t *)s_drivers[4]) == BUTTON_INACTIVE);
    assert(s_keypad_boot_diag_log_count == 2U);

    s_callbacks[4](&s_buttons[4], s_callback_user_data[4]);
    assert(s_boot_callback_calls == 1U);
    assert(s_boot_callback_key == KEYPAD_KEY_OK);
    assert(s_keypad_events_posted == 0);
    assert(s_keypad_log_count == 1U);
    assert(strcmp(s_last_keypad_log, "KEYPAD_BUTTON_PRESS key=OK value=5") == 0);

    keypad_button_adapter_stop_boot();
    s_callbacks[3](&s_buttons[3], s_callback_user_data[3]);
    assert(s_boot_callback_calls == 1U);
    assert(s_keypad_events_posted == 1);
    assert(s_last_keypad_event_value == KEYPAD_KEY_RIGHT);
    assert(s_keypad_log_count == 2U);
    assert(strcmp(s_last_keypad_log, "KEYPAD_BUTTON_PRESS key=RIGHT value=4") == 0);

    keypad_button_adapter_stop();
}


static void test_adc_polling_is_quiet_until_press_registration(void)
{
    reset_test_state();
    assert(keypad_button_adapter_init() == ESP_OK);
    s_raw_value = 0;
    assert(s_drivers[4]->get_key_level((button_driver_t *)s_drivers[4]) == BUTTON_INACTIVE);

    s_now_us += 100000;
    s_raw_value = 2873;
    assert(s_drivers[4]->get_key_level((button_driver_t *)s_drivers[4]) == BUTTON_INACTIVE);

    s_now_us += 1000000;
    s_raw_value = 0;
    assert(s_drivers[4]->get_key_level((button_driver_t *)s_drivers[4]) == BUTTON_INACTIVE);

    s_now_us += 1000001;
    s_raw_value = 2873;
    assert(s_drivers[4]->get_key_level((button_driver_t *)s_drivers[4]) == BUTTON_INACTIVE);
    assert(s_keypad_log_count == 0U);

    s_now_us += 1001;
    s_raw_value = 4095;
    assert(s_drivers[4]->get_key_level((button_driver_t *)s_drivers[4]) == BUTTON_INACTIVE);
    for (unsigned sample = 1U; sample < 3U; ++sample) {
        s_now_us += 1001;
        assert(s_drivers[4]->get_key_level((button_driver_t *)s_drivers[4]) == BUTTON_INACTIVE);
    }
    s_now_us += 1000000;
    s_raw_value = 2873;
    assert(s_drivers[4]->get_key_level((button_driver_t *)s_drivers[4]) == BUTTON_ACTIVE);
    assert(s_keypad_log_count == 0U);
    /* The three 4095 samples settled; this is the first OK sample. */
    assert(s_keypad_transition_log_count == 1U);
    assert(s_keypad_boot_diag_log_count == 0U);

    /* Raw key detection is not registration: only the vendor PRESS_DOWN
     * callback produces the named press log. */
    s_callbacks[4](&s_buttons[4], s_callback_user_data[4]);
    assert(s_keypad_log_count == 1U);
    assert(strcmp(s_last_keypad_log, "KEYPAD_BUTTON_PRESS key=OK value=5") == 0);

    /* Neither holding a key nor released ADC polling emits periodic logs. */
    for (unsigned second = 0U; second < 10U; ++second) {
        s_now_us += 1000001;
        assert(s_drivers[4]->get_key_level((button_driver_t *)s_drivers[4]) == BUTTON_ACTIVE);
        assert(s_keypad_log_count == 1U);
    }
    assert(s_keypad_transition_log_count == 2U);
    assert(strstr(s_last_transition_log, "decoded=OK") != NULL);
    s_raw_value = 4095;
    for (unsigned second = 0U; second < 10U; ++second) {
        s_now_us += 1000001;
        assert(s_drivers[4]->get_key_level((button_driver_t *)s_drivers[4]) == BUTTON_INACTIVE);
        assert(s_keypad_log_count == 1U);
    }
    /* Three 4095 samples settle the return to the released level. */
    assert(s_keypad_transition_log_count == 3U);
    assert(strstr(s_last_transition_log, "decoded=NONE") != NULL);
    assert(strstr(s_last_transition_log, "release_armed=1") != NULL);
    keypad_button_adapter_stop();

    /* A blocked candidate after restart still must not be called a press. */
    reset_test_state();
    assert(keypad_button_adapter_init() == ESP_OK);
    s_now_us = 2000000;
    s_raw_value = 550;
    assert(s_drivers[1]->get_key_level((button_driver_t *)s_drivers[1]) == BUTTON_INACTIVE);
    assert(s_keypad_log_count == 0U);
    keypad_button_adapter_stop();
}


static uint8_t poll_raw(int raw)
{
    s_raw_value = raw;
    s_now_us += 10000LL;
    return s_drivers[0]->get_key_level((button_driver_t *)s_drivers[0]);
}


static void test_settled_level_boundaries_and_pending_run(void)
{
    reset_test_state();
    assert(keypad_button_adapter_init() == ESP_OK);

    (void)poll_raw(150); /* Initial level establishes the baseline silently. */
    assert(s_keypad_transition_log_count == 0U);
    for (unsigned i = 0U; i < 3U; ++i) {
        (void)poll_raw(249); /* Delta 99 never starts a settle run. */
    }
    assert(s_keypad_transition_log_count == 0U);

    (void)poll_raw(250); /* Delta 100, first settled sample. */
    (void)poll_raw(250);
    assert(s_keypad_transition_log_count == 0U);
    (void)poll_raw(250);
    assert(s_keypad_transition_log_count == 1U);
    assert(strcmp(
        s_last_transition_log,
        "KEYPAD_RAW_TRANSITION raw=250 from=NONE to=NONE decoded=NONE release_armed=0"
    ) == 0);

    for (unsigned i = 0U; i < 12U; ++i) {
        (void)poll_raw(i % 2U == 0U ? 0 : 150);
    }
    assert(s_keypad_transition_log_count == 1U);

    (void)poll_raw(0);   /* Start a candidate run away from the logged 250. */
    (void)poll_raw(200); /* Within 100 of 250: discard the pending run. */
    (void)poll_raw(0);
    (void)poll_raw(0);
    assert(s_keypad_transition_log_count == 1U);
    (void)poll_raw(0);
    assert(s_keypad_transition_log_count == 2U);
    assert(strstr(s_last_transition_log, "raw=0 from=NONE to=LEFT") != NULL);
    keypad_button_adapter_stop();
}


static void test_settled_level_ignores_errors_and_resets_on_restart(void)
{
    reset_test_state();
    assert(keypad_button_adapter_init() == ESP_OK);
    (void)poll_raw(150);
    (void)poll_raw(300);
    assert(s_keypad_transition_log_count == 0U);

    s_service_busy = true;
    (void)poll_raw(300);
    s_service_busy = false;
    assert(s_keypad_transition_log_count == 0U);
    (void)poll_raw(300);
    assert(s_keypad_transition_log_count == 0U);

    s_service_error = ESP_FAIL;
    (void)poll_raw(300);
    s_service_error = ESP_OK;
    assert(s_keypad_transition_log_count == 0U);
    (void)poll_raw(300);
    assert(s_keypad_transition_log_count == 1U);

    keypad_button_adapter_stop();
    reset_test_state();
    assert(keypad_button_adapter_init() == ESP_OK);
    (void)poll_raw(0); /* First sample after stop/init is silent again. */
    assert(s_keypad_transition_log_count == 0U);
    keypad_button_adapter_stop();
}


static void test_settled_level_slow_ramp_and_gate_open(void)
{
    reset_test_state();
    assert(keypad_button_adapter_init() == ESP_OK);
    for (unsigned sample = 0U; sample <= 2500U; ++sample) {
        s_raw_value = (int)(sample / 10U); /* +10 raw/s at 100 Hz. */
        s_now_us += 10000LL;
        (void)s_drivers[0]->get_key_level((button_driver_t *)s_drivers[0]);
    }
    assert(s_keypad_transition_log_count <= 3U);
    assert(s_keypad_transition_log_count == 2U);
    for (unsigned i = 1U; i < s_keypad_transition_log_count; ++i) {
        assert(s_transition_log_times[i] - s_transition_log_times[i - 1U]
            >= 9000000LL);
    }
    assert(s_keypad_boot_diag_log_count == 0U);

    keypad_button_adapter_stop();
    reset_test_state();
    assert(keypad_button_adapter_init() == ESP_OK);
    (void)poll_raw(4095);
    (void)poll_raw(4095);
    (void)poll_raw(4095);
    (void)poll_raw(3340);
    (void)poll_raw(3340);
    assert(s_keypad_transition_log_count == 0U);
    (void)poll_raw(3340);
    assert(s_keypad_transition_log_count == 1U);
    assert(strstr(s_last_transition_log, "to=OK decoded=OK") != NULL);
    (void)poll_raw(4095);
    (void)poll_raw(4095);
    assert(s_keypad_transition_log_count == 1U);
    (void)poll_raw(4095);
    assert(s_keypad_transition_log_count == 2U);
    assert(strstr(s_last_transition_log, "to=NONE decoded=NONE") != NULL);
    assert(strstr(s_last_transition_log, "release_armed=1") != NULL);
    keypad_button_adapter_stop();
}


static void test_idle_warning_delay_rate_limit_and_armed_lifecycle(void)
{
    reset_test_state();
    assert(keypad_button_adapter_init() == ESP_OK);
    s_now_us = 10000000LL;
    int context_token = 0;
    s_boot_callback_expected_context = &context_token;
    assert(keypad_button_adapter_start_boot(
        boot_prompt_callback,
        &context_token
    ) == ESP_OK);

    for (unsigned sample = 0U; sample < 299U; ++sample) {
        (void)poll_raw(150);
    }
    assert(s_keypad_idle_warning_log_count == 0U);
    (void)poll_raw(150);
    assert(s_keypad_idle_warning_log_count == 1U);
    assert(strcmp(
        s_last_idle_warning_log,
        "KEYPAD_IDLE_LEVEL_INVALID raw=150 need>=3600: key line is not pulled high - check keypad module + (3V3), - (GND) and S (GPIO8) wiring; no key can be detected"
    ) == 0);

    for (unsigned sample = 0U; sample < 499U; ++sample) {
        (void)poll_raw(150);
    }
    assert(s_keypad_idle_warning_log_count == 1U);
    (void)poll_raw(150);
    assert(s_keypad_idle_warning_log_count == 2U);

    keypad_button_adapter_stop_boot();
    assert(keypad_button_adapter_start_boot(
        boot_prompt_callback,
        &context_token
    ) == ESP_OK);
    for (unsigned sample = 0U; sample < 299U; ++sample) {
        (void)poll_raw(150);
    }
    assert(s_keypad_idle_warning_log_count == 2U);
    (void)poll_raw(150);
    assert(s_keypad_idle_warning_log_count == 3U);

    (void)poll_raw(4000);
    (void)poll_raw(4000);
    (void)poll_raw(4000);
    assert(strstr(s_last_transition_log, "raw=4000") != NULL);
    assert(strstr(s_last_transition_log, "release_armed=1") != NULL);
    (void)poll_raw(3340);
    (void)poll_raw(3340);
    (void)poll_raw(3340);
    assert(strstr(s_last_transition_log, "raw=3340") != NULL);
    assert(strstr(s_last_transition_log, "release_armed=0") != NULL);
    s_callbacks[4](&s_buttons[4], s_callback_user_data[4]);
    assert(s_boot_callback_calls == 1U);
    for (unsigned sample = 0U; sample < 600U; ++sample) {
        (void)poll_raw(3340);
    }
    assert(s_keypad_idle_warning_log_count == 3U);

    keypad_button_adapter_stop_boot();
    assert(keypad_button_adapter_start_boot(
        boot_prompt_callback,
        &context_token
    ) == ESP_OK);
    for (unsigned sample = 0U; sample < 299U; ++sample) {
        (void)poll_raw(3340);
    }
    assert(s_keypad_idle_warning_log_count == 3U);
    (void)poll_raw(3340);
    assert(s_keypad_idle_warning_log_count == 4U);

    keypad_button_adapter_stop();
    reset_test_state();
    assert(keypad_button_adapter_init() == ESP_OK);
    s_now_us = 70000000LL;
    s_boot_callback_expected_context = &context_token;
    assert(keypad_button_adapter_start_boot(
        boot_prompt_callback,
        &context_token
    ) == ESP_OK);
    for (unsigned sample = 0U; sample < 299U; ++sample) {
        (void)poll_raw(150);
    }
    assert(s_keypad_idle_warning_log_count == 0U);
    (void)poll_raw(150);
    assert(s_keypad_idle_warning_log_count == 1U);
    keypad_button_adapter_stop();

    reset_test_state();
    assert(keypad_button_adapter_init() == ESP_OK);
    for (unsigned sample = 0U; sample < 500U; ++sample) {
        (void)poll_raw(150);
    }
    assert(s_keypad_idle_warning_log_count == 0U);
    keypad_button_adapter_stop();
}


static void test_idle_warning_latches_gate_armed_before_boot_start(void)
{
    reset_test_state();
    assert(keypad_button_adapter_init() == ESP_OK);
    (void)poll_raw(4000);
    (void)poll_raw(4000);
    (void)poll_raw(4000);

    int context_token = 0;
    s_boot_callback_expected_context = &context_token;
    assert(keypad_button_adapter_start_boot(
        boot_prompt_callback,
        &context_token
    ) == ESP_OK);
    /* The first monitored key sample closes the previously armed gate. */
    for (unsigned sample = 0U; sample < 400U; ++sample) {
        (void)poll_raw(3340);
    }
    assert(s_keypad_idle_warning_log_count == 0U);
    keypad_button_adapter_stop();
}

int main(void)
{
    test_keypad_buttons_use_one_broker_sample();
    test_debounced_button_callback_posts_keypad_event();
    test_boot_callback_routes_only_while_prompt_is_active();
    test_adc_polling_is_quiet_until_press_registration();
    test_settled_level_boundaries_and_pending_run();
    test_settled_level_ignores_errors_and_resets_on_restart();
    test_settled_level_slow_ramp_and_gate_open();
    test_idle_warning_delay_rate_limit_and_armed_lifecycle();
    test_idle_warning_latches_gate_armed_before_boot_start();
    puts("keypad ADC ownership tests passed");
    return 0;
}
