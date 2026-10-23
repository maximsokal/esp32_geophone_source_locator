#include <assert.h>
#include "freertos/semphr.h"
#include "seismic_acquisition.h"
#include "seismic_buffer.h"
#include "seismic_source_driver.h"

struct test_semaphore { bool held; };
static struct test_semaphore s_mutex;
static unsigned s_starts;
static unsigned s_stops;
static const float s_values[SEISMIC_CHANNEL_COUNT] = {1, 2, 3, 4};

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &s_mutex; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t timeout)
{
    (void)timeout;
    assert(mutex == &s_mutex && !mutex->held);
    mutex->held = true;
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex)
{
    assert(mutex == &s_mutex && mutex->held);
    mutex->held = false;
    return pdTRUE;
}
int64_t esp_timer_get_time(void) { return 1000000; }
esp_err_t device_status_set_acquisition(bool running, uint32_t rate, uint64_t samples, uint64_t ms)
{
    (void)running; (void)rate; (void)samples; (void)ms;
    return ESP_OK;
}
static esp_err_t producer_start(void)
{
    ++s_starts;
    assert(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, 20000U) == ESP_OK);
    for (uint64_t i = 0U; i < 3U; ++i) {
        assert(seismic_buffer_push_indexed(1000000 + (int64_t)i * 50, i, s_values) == ESP_OK);
    }
    return ESP_OK;
}
static void producer_stop(void)
{
    ++s_stops;
    /* The AD7606 worker publishes its last completed conversion during stop. */
    assert(seismic_buffer_push_indexed(1000150, 3U, s_values) == ESP_OK);
}
static const seismic_source_driver_t s_driver = {
    .name = "TAIL_TEST", .sample_rate_hz = 20000U, .channel_count = 4U,
    .start = producer_start, .stop = producer_stop,
};
const seismic_source_driver_t *seismic_source_registry_selected(void) { return &s_driver; }

int main(void)
{
    assert(seismic_buffer_init() == ESP_OK);
    assert(seismic_acquisition_init() == ESP_OK);
    assert(seismic_acquisition_start() == ESP_OK);
    seismic_frame_t frames[4];
    uint64_t indices[4], cursor = 0U, dropped = 0U;
    size_t count = 0U;
    seismic_buffer_info_t info;
    assert(seismic_buffer_copy_since_with_source_indices(
        &cursor, frames, indices, 1U, &count, &dropped, &info) == ESP_OK);
    assert(count == 1U && cursor == 1U && indices[0] == 0U);

    seismic_acquisition_prepare_shutdown();
    seismic_acquisition_poll(false);
    assert(!seismic_acquisition_is_running());
    assert(seismic_acquisition_start() == ESP_ERR_INVALID_STATE);
    assert(s_starts == 1U && s_stops == 1U);
    /* Drain the same ring/cursor API used by the production recorder. */
    assert(seismic_buffer_copy_since_with_source_indices(
        &cursor, frames, indices, 4U, &count, &dropped, &info) == ESP_OK);
    assert(count == 3U && dropped == 0U && cursor == 4U);
    assert(info.source == SEISMIC_SOURCE_AD7606 && info.total_frames == 4U);
    for (size_t i = 0U; i < count; ++i) {
        assert(indices[i] == i + 1U);
        assert(frames[i].timestamp_us == 1000050 + (int64_t)i * 50);
        assert(frames[i].channel_uv[3] == 4.0f);
    }
    seismic_acquisition_stop();
    assert(seismic_buffer_copy_latest(frames, 4U, &count, &info) == ESP_ERR_INVALID_STATE);
    assert(seismic_buffer_capacity() == 0U);
    return 0;
}
