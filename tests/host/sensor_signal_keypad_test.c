#define main keypad_baseline_main
#include "../../components/keypad/tests/keypad_decode_test.c"
#undef main

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static void test_keypad_whole_adc_and_previous_key_domains(void)
{
    const struct { int low, high; keypad_key_t key; } windows[] = {
        {0, 115, KEYPAD_KEY_LEFT}, {435, 745, KEYPAD_KEY_UP},
        {1065, 1445, KEYPAD_KEY_DOWN}, {1765, 2251, KEYPAD_KEY_RIGHT},
        {2571, 3484, KEYPAD_KEY_OK},
    };
    for (int raw = -1; raw <= 4096; ++raw) {
        keypad_key_t decoded = KEYPAD_KEY_NONE;
        for (unsigned window = 0U; window < 5U; ++window) {
            if (raw >= windows[window].low && raw <= windows[window].high) decoded = windows[window].key;
        }
        assert(keypad_decode_raw(raw) == decoded);
        for (int prior = -1; prior <= UINT8_MAX; ++prior) {
            const bool retain = decoded == KEYPAD_KEY_NONE && raw >= 0 && raw <= 3484 &&
                prior >= KEYPAD_KEY_LEFT && prior <= KEYPAD_KEY_OK;
            assert(keypad_decode_raw_with_previous(raw, (keypad_key_t)prior) ==
                (retain ? (keypad_key_t)prior : decoded));
        }
    }
    assert(keypad_decode_raw_with_previous(INT_MIN, KEYPAD_KEY_OK) == KEYPAD_KEY_NONE);
    assert(keypad_decode_raw_with_previous(INT_MAX, KEYPAD_KEY_OK) == KEYPAD_KEY_NONE);
    puts("SIGNAL_CASE KEYPAD.ADC_PRIOR PASS raw=4098 prior=257");
}

int main(void)
{
    assert(keypad_baseline_main() == 0);
    test_keypad_whole_adc_and_previous_key_domains();
    return 0;
}
