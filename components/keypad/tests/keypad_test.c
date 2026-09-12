#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "keypad.h"


static int adapter_init_calls;
static int adapter_stop_calls;


esp_err_t adc_service_init(void)
{
    return ESP_OK;
}


esp_err_t keypad_button_adapter_init(void)
{
    adapter_init_calls++;
    return ESP_OK;
}


esp_err_t keypad_button_adapter_start(
    keypad_event_callback_t callback,
    void *user_context
)
{
    (void)callback;
    (void)user_context;
    return ESP_OK;
}


void keypad_button_adapter_stop(void)
{
    adapter_stop_calls++;
}


esp_err_t keypad_button_adapter_read_key(keypad_key_t *key)
{
    if (key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *key = KEYPAD_KEY_OK;
    return ESP_OK;
}


static void test_names(void)
{
    assert(strcmp(keypad_key_name(KEYPAD_KEY_LEFT), "LEFT") == 0);
    assert(strcmp(keypad_key_name(KEYPAD_KEY_UP), "UP") == 0);
    assert(strcmp(keypad_key_name(KEYPAD_KEY_DOWN), "DOWN") == 0);
    assert(strcmp(keypad_key_name(KEYPAD_KEY_RIGHT), "RIGHT") == 0);
    assert(strcmp(keypad_key_name(KEYPAD_KEY_OK), "OK") == 0);
    assert(strcmp(keypad_key_name(KEYPAD_KEY_NONE), "NONE") == 0);
}


static void test_lifecycle_is_adapter_owned(void)
{
    assert(keypad_start_event_monitor(NULL, NULL) == ESP_ERR_INVALID_STATE);
    assert(keypad_init() == ESP_OK);
    assert(adapter_init_calls == 1);
    assert(keypad_start_event_monitor(NULL, NULL) == ESP_OK);
    keypad_key_t key = KEYPAD_KEY_NONE;
    assert(keypad_read_key(&key) == ESP_OK);
    assert(key == KEYPAD_KEY_OK);
    keypad_stop_event_monitor();
    assert(adapter_stop_calls == 1);
}


int main(void)
{
    test_names();
    test_lifecycle_is_adapter_owned();
    puts("keypad tests passed");
    return 0;
}
