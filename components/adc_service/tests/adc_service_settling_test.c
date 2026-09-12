#include <assert.h>
#include <stdio.h>

#include "adc_service.h"
#include "adc_service_internal.h"


adc_oneshot_unit_handle_t g_adc_service_handle = (void *)0x1;
SemaphoreHandle_t g_adc_service_mutex = (void *)0x2;
bool g_adc_service_initialized = true;


typedef struct {
    adc_channel_t channel;
    int value;
} read_record_t;


static read_record_t s_reads[8];
static size_t s_read_count;


BaseType_t xSemaphoreTake(
    SemaphoreHandle_t semaphore,
    TickType_t ticks_to_wait
)
{
    assert(semaphore == g_adc_service_mutex);
    assert(ticks_to_wait == portMAX_DELAY);
    return pdTRUE;
}


BaseType_t xSemaphoreGive(
    SemaphoreHandle_t semaphore
)
{
    assert(semaphore == g_adc_service_mutex);
    return pdTRUE;
}


esp_err_t adc_oneshot_read(
    adc_oneshot_unit_handle_t handle,
    adc_channel_t channel,
    int *raw_value
)
{
    static const int values[] = { 100, 101, 200, 201, 300, 301 };

    assert(handle == g_adc_service_handle);
    assert(raw_value != NULL);
    assert(s_read_count < sizeof(values) / sizeof(values[0]));

    s_reads[s_read_count].channel = channel;
    s_reads[s_read_count].value = values[s_read_count];
    *raw_value = values[s_read_count];
    ++s_read_count;
    return ESP_OK;
}


static void test_each_service_read_settles_after_channel_switch(void)
{
    int value = 0;

    s_read_count = 0;

    /* Service read A: discard 100, return the subsequent 101. */
    assert(adc_service_read_raw(1, &value) == ESP_OK);
    assert(value == 101);

    /* Service read B has its own discard, then A has another one after the
     * channel switch. No application code can borrow the ADC1 handle. */
    assert(adc_service_read_raw(2, &value) == ESP_OK);
    assert(value == 201);
    assert(adc_service_read_raw(1, &value) == ESP_OK);
    assert(value == 301);

    assert(s_read_count == 6U);
    assert(s_reads[0].channel == 1);
    assert(s_reads[1].channel == 1);
    assert(s_reads[2].channel == 2);
    assert(s_reads[3].channel == 2);
    assert(s_reads[4].channel == 1);
    assert(s_reads[5].channel == 1);
}


int main(void)
{
    test_each_service_read_settles_after_channel_switch();
    puts("adc service settling tests passed");
    return 0;
}
