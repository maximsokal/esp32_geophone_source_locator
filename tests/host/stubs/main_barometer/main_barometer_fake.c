#include "main_barometer_fake.h"

#include <string.h>

main_barometer_fake_t g_main_barometer_fake;

void main_barometer_fake_reset(void)
{
    memset(&g_main_barometer_fake, 0, sizeof(g_main_barometer_fake));
    g_main_barometer_fake.poll_result = ESP_OK;
    g_main_barometer_fake.set_main_result = ESP_OK;
}

void main_barometer_init(main_barometer_t *barometer)
{
    memset(barometer, 0, sizeof(*barometer));
    ++g_main_barometer_fake.init_calls;
}

esp_err_t main_barometer_poll(main_barometer_t *barometer, uint64_t now_ms)
{
    ++g_main_barometer_fake.poll_calls;
    g_main_barometer_fake.last_poll_ms = now_ms;
    if (g_main_barometer_fake.on_poll != NULL) {
        g_main_barometer_fake.on_poll();
    }
    if (g_main_barometer_fake.next_poll_delay_ms != 0U) {
        barometer->handles.bus = (void *)barometer;
        barometer->next_bmp280_poll_ms = now_ms + g_main_barometer_fake.next_poll_delay_ms;
    }
    barometer->reading = g_main_barometer_fake.reading;
    return g_main_barometer_fake.poll_result;
}

uint32_t main_barometer_ms_until_poll(
    const main_barometer_t *barometer,
    uint64_t now_ms,
    uint32_t max_ms)
{
    if (barometer == NULL || barometer->handles.bus == NULL ||
        !g_main_barometer_fake.polling_requested) {
        return max_ms;
    }
    const uint64_t until_ms = barometer->next_bmp280_poll_ms > now_ms
        ? barometer->next_bmp280_poll_ms - now_ms : 1U;
    return until_ms < max_ms ? (uint32_t)until_ms : max_ms;
}

void main_barometer_close(main_barometer_t *barometer)
{
    (void)barometer;
    ++g_main_barometer_fake.close_calls;
}

void main_barometer_get_reading(
    const main_barometer_t *barometer,
    main_barometer_reading_t *reading)
{
    *reading = barometer->reading;
}

esp_err_t device_status_set_main(const device_main_status_t *main_status)
{
    ++g_main_barometer_fake.set_main_calls;
    if (g_main_barometer_fake.set_main_result == ESP_OK) {
        g_main_barometer_fake.last_main_status = *main_status;
    }
    return g_main_barometer_fake.set_main_result;
}
