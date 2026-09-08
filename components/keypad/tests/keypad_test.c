#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#include "esp_err.h"
#include "keypad_event_internal.h"
#include "keypad_internal.h"

static keypad_event_state_t test_state;
static const keypad_key_t *test_keys;
static size_t test_key_count;
static size_t test_key_index;
static keypad_key_t delivered[8];
static size_t delivered_count;

keypad_event_state_t *keypad_event_state(void)
{
    return &test_state;
}

const char *keypad_key_name(keypad_key_t key)
{
    (void)key;
    return "TEST";
}

esp_err_t keypad_read_key(keypad_key_t *key)
{
    assert(key != NULL);
    if (test_key_index >= test_key_count) {
        test_state.running = false;
        return ESP_ERR_TIMEOUT;
    }
    *key = test_keys[test_key_index++];
    return ESP_OK;
}

static void callback(keypad_key_t key, void *context)
{
    (void)context;
    assert(delivered_count < sizeof(delivered) / sizeof(delivered[0]));
    delivered[delivered_count++] = key;
}

static void test_decode_boundaries(void)
{
    assert(keypad_decode_raw(0) == KEYPAD_KEY_LEFT);
    assert(keypad_decode_raw(275) == KEYPAD_KEY_LEFT);
    assert(keypad_decode_raw(276) == KEYPAD_KEY_UP);
    assert(keypad_decode_raw(905) == KEYPAD_KEY_UP);
    assert(keypad_decode_raw(906) == KEYPAD_KEY_DOWN);
    assert(keypad_decode_raw(1605) == KEYPAD_KEY_DOWN);
    assert(keypad_decode_raw(1606) == KEYPAD_KEY_RIGHT);
    assert(keypad_decode_raw(2411) == KEYPAD_KEY_RIGHT);
    assert(keypad_decode_raw(2412) == KEYPAD_KEY_OK);
    assert(keypad_decode_raw(3484) == KEYPAD_KEY_OK);
    assert(keypad_decode_raw(3485) == KEYPAD_KEY_NONE);
    assert(keypad_decode_raw(4095) == KEYPAD_KEY_NONE);
    assert(keypad_decode_raw(-1) == KEYPAD_KEY_LEFT);
}

static void test_debounce_delivers_one_event_per_stable_transition(void)
{
    const keypad_key_t sequence[] = {
        KEYPAD_KEY_LEFT,
        KEYPAD_KEY_LEFT,
        KEYPAD_KEY_UP,
        KEYPAD_KEY_LEFT,
        KEYPAD_KEY_LEFT,
        KEYPAD_KEY_LEFT,
        KEYPAD_KEY_NONE,
        KEYPAD_KEY_NONE,
        KEYPAD_KEY_NONE,
        KEYPAD_KEY_RIGHT,
        KEYPAD_KEY_RIGHT,
        KEYPAD_KEY_RIGHT,
    };

    test_state = (keypad_event_state_t) {
        .task = (TaskHandle_t)1,
        .callback = callback,
        .user_context = NULL,
        .running = true,
    };
    test_keys = sequence;
    test_key_count = sizeof(sequence) / sizeof(sequence[0]);
    test_key_index = 0;
    delivered_count = 0;

    keypad_event_worker(NULL);

    assert(delivered_count == 2);
    assert(delivered[0] == KEYPAD_KEY_LEFT);
    assert(delivered[1] == KEYPAD_KEY_RIGHT);
    assert(test_state.task == NULL);
}

int main(void)
{
    test_decode_boundaries();
    test_debounce_delivers_one_event_per_stable_transition();
    puts("keypad tests passed");
    return 0;
}
