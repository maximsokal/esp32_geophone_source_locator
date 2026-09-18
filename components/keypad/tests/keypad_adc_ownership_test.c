#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

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
static int s_try_reads;
static int s_direct_adc_reads;
static int s_keypad_events_posted;
static int32_t s_last_keypad_event_value;


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
    s_try_reads = 0;
    s_direct_adc_reads = 0;
    s_now_us = 10000;
    s_raw_value = 550;
    s_keypad_events_posted = 0;
    s_last_keypad_event_value = 0;

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
    assert(s_drivers[1]->get_key_level((button_driver_t *)s_drivers[1]) == BUTTON_INACTIVE);
    assert(s_service_reads == 1);
    assert(s_last_sample_count == 16U);
    assert(s_try_reads == 1);

    s_now_us += 1001;
    s_raw_value = 4095;
    assert(s_drivers[1]->get_key_level((button_driver_t *)s_drivers[1]) == BUTTON_INACTIVE);

    s_now_us += 1001;
    s_raw_value = 550;

    /* A calibrated UP value must be active, and the first level query must go
     * through the service with the same 16-sample depth as the compatibility
     * read path. */
    assert(s_drivers[1]->get_key_level((button_driver_t *)s_drivers[1]) == BUTTON_ACTIVE);
    assert(s_service_reads == 3);
    assert(s_last_sample_count == 16U);
    assert(s_try_reads == 3);

    /* Every Button object in the same timer tick sees the same broker-owned
     * conversion; no second direct ADC read is allowed. */
    assert(s_drivers[0]->get_key_level((button_driver_t *)s_drivers[0]) == BUTTON_INACTIVE);
    assert(s_drivers[4]->get_key_level((button_driver_t *)s_drivers[4]) == BUTTON_INACTIVE);
    assert(s_service_reads == 3);
    assert(s_direct_adc_reads == 0);

    /* The old direct threshold decoder turned these adjacent readings into a
     * DOWN transition. The boundary deadband must retain UP and keep DOWN
     * inactive instead. */
    s_now_us += 1001;
    s_raw_value = 899;
    assert(s_drivers[1]->get_key_level((button_driver_t *)s_drivers[1]) == BUTTON_ACTIVE);
    assert(s_drivers[2]->get_key_level((button_driver_t *)s_drivers[2]) == BUTTON_INACTIVE);
    assert(s_service_reads == 4);

    s_now_us += 1001;
    s_raw_value = 906;
    assert(s_drivers[1]->get_key_level((button_driver_t *)s_drivers[1]) == BUTTON_ACTIVE);
    assert(s_drivers[2]->get_key_level((button_driver_t *)s_drivers[2]) == BUTTON_INACTIVE);
    assert(s_service_reads == 5);

    /* If the timer task finds the broker busy, preserve the last valid key
     * rather than synthesizing a release. A later logical Button callback in
     * this same timer tick must not retry the ADC read. */
    s_now_us += 1001;
    s_service_busy = true;
    assert(s_drivers[1]->get_key_level((button_driver_t *)s_drivers[1]) == BUTTON_ACTIVE);
    assert(s_drivers[2]->get_key_level((button_driver_t *)s_drivers[2]) == BUTTON_INACTIVE);
    assert(s_service_reads == 5);
    assert(s_try_reads == 6);

    /* After the failed-attempt window expires, acquire the new raw value. The
     * fake broker advances time during the read so the cache timestamp must
     * be taken after completion; the next Button object still reuses it. */
    s_now_us += 1001;
    s_service_busy = false;
    s_read_duration_us = 1500;
    s_raw_value = 1261;
    assert(s_drivers[2]->get_key_level((button_driver_t *)s_drivers[2]) == BUTTON_ACTIVE);
    assert(s_service_reads == 6);
    assert(s_try_reads == 7);
    assert(s_drivers[3]->get_key_level((button_driver_t *)s_drivers[3]) == BUTTON_INACTIVE);
    assert(s_service_reads == 6);
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
    }

    keypad_button_adapter_stop();
}


int main(void)
{
    test_keypad_buttons_use_one_broker_sample();
    test_debounced_button_callback_posts_keypad_event();
    puts("keypad ADC ownership tests passed");
    return 0;
}
