#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "spi_bus.h"
#include "spi_bus_timing_log.h"

static unsigned gpio_config_calls;
static unsigned gpio_set_calls;
static unsigned spi_init_calls;
static unsigned mutex_take_calls;
static unsigned mutex_give_calls;
static esp_err_t gpio_config_result;
static esp_err_t gpio_set_result;
static esp_err_t spi_init_result;
static gpio_num_t last_gpio_pin;
static uint32_t last_gpio_level;
static gpio_config_t last_gpio_config;
static gpio_num_t gpio_set_history[4];
static uint32_t gpio_level_history[4];
static spi_bus_config_t last_bus_config;
static int last_dma_channel;
static unsigned quiesce_calls;
static unsigned resume_calls;
static esp_err_t quiesce_result;
static int callback_context;
static SemaphoreHandle_t fake_mutex = (SemaphoreHandle_t)&callback_context;
static BaseType_t mutex_take_result = pdTRUE;
static TickType_t last_mutex_timeout;

esp_err_t gpio_config(const gpio_config_t *config)
{
    assert(config != NULL);
    ++gpio_config_calls;
    last_gpio_config = *config;
    return gpio_config_result;
}

esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    ++gpio_set_calls;
    last_gpio_pin = pin;
    last_gpio_level = level;
    if (gpio_set_calls <= 4U) {
        gpio_set_history[gpio_set_calls - 1U] = pin;
        gpio_level_history[gpio_set_calls - 1U] = level;
    }
    return gpio_set_result;
}

esp_err_t spi_bus_initialize(
    spi_host_device_t host_id,
    const spi_bus_config_t *bus_config,
    int dma_chan
)
{
    assert(host_id == SPI2_HOST);
    assert(bus_config != NULL);
    ++spi_init_calls;
    last_bus_config = *bus_config;
    last_dma_channel = dma_chan;
    return spi_init_result;
}

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void)
{
    return fake_mutex;
}

BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t semaphore, TickType_t timeout)
{
    assert(semaphore == fake_mutex);
    last_mutex_timeout = timeout;
    ++mutex_take_calls;
    return mutex_take_result;
}

BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t semaphore)
{
    assert(semaphore == fake_mutex);
    ++mutex_give_calls;
    return pdTRUE;
}

static esp_err_t quiesce(void *context)
{
    assert(context == &callback_context);
    ++quiesce_calls;
    return quiesce_result;
}

static void resume(void *context)
{
    assert(context == &callback_context);
    ++resume_calls;
}

static void reset_counters(void)
{
    gpio_config_calls = 0U;
    gpio_set_calls = 0U;
    spi_init_calls = 0U;
    mutex_take_calls = 0U;
    mutex_give_calls = 0U;
    quiesce_calls = 0U;
    resume_calls = 0U;
    gpio_config_result = ESP_OK;
    gpio_set_result = ESP_OK;
    spi_init_result = ESP_OK;
    quiesce_result = ESP_OK;
    last_gpio_pin = -1;
    last_gpio_level = 0U;
    last_gpio_config = (gpio_config_t){0};
    for (size_t i = 0U; i < 4U; ++i) {
        gpio_set_history[i] = -1;
        gpio_level_history[i] = 0U;
    }
    last_dma_channel = 0;
}

static void test_init_sets_safe_levels_and_bus_config(void)
{
    reset_counters();
    assert(app_spi_bus_init() == ESP_OK);
    assert(gpio_config_calls == 1U);
    assert(gpio_set_calls == 2U);
    assert(
        last_gpio_config.pin_bit_mask ==
        ((1ULL << GPIO_NUM_14) | (1ULL << GPIO_NUM_17))
    );
    assert(last_gpio_config.mode == GPIO_MODE_INPUT_OUTPUT);
    assert((last_gpio_config.mode & GPIO_MODE_DEF_INPUT) != 0);
    assert(gpio_set_history[0] == GPIO_NUM_14);
    assert(gpio_level_history[0] == 1U);
    assert(gpio_set_history[1] == GPIO_NUM_17);
    assert(gpio_level_history[1] == 1U);
    assert(last_gpio_pin == GPIO_NUM_17);
    assert(last_gpio_level == 1U);
    assert(spi_init_calls == 1U);
    assert(last_bus_config.mosi_io_num == GPIO_NUM_11);
    assert(last_bus_config.miso_io_num == GPIO_NUM_13);
    assert(last_bus_config.sclk_io_num == GPIO_NUM_12);
    assert(last_dma_channel == SPI_DMA_CH_AUTO);
    assert(app_spi_bus_get_host() == SPI2_HOST);

    /* A pre-initialized bus is an idempotent success. */
    spi_init_result = ESP_ERR_INVALID_STATE;
    assert(app_spi_bus_init() == ESP_OK);
}

static void test_guard_pairs_quiesce_and_resume(void)
{
    reset_counters();
    assert(app_spi_bus_register_display_guard(NULL, resume, &callback_context) == ESP_ERR_INVALID_ARG);
    assert(app_spi_bus_register_display_guard(quiesce, NULL, &callback_context) == ESP_ERR_INVALID_ARG);
    assert(app_spi_bus_register_display_guard(quiesce, resume, &callback_context) == ESP_OK);

    assert(app_spi_bus_sd_operation_begin() == ESP_OK);
    assert(quiesce_calls == 1U);
    assert(resume_calls == 0U);
    app_spi_bus_sd_operation_end();
    assert(resume_calls == 1U);
    assert(mutex_take_calls == 2U); /* register + operation begin */
    assert(mutex_give_calls == 2U); /* operation end + register */

    app_spi_bus_unregister_display_guard();
    assert(app_spi_bus_sd_operation_begin() == ESP_OK);
    app_spi_bus_sd_operation_end();
    assert(quiesce_calls == 1U);
    assert(resume_calls == 1U);
}

static void test_quiesce_failure_releases_mutex_without_resume(void)
{
    reset_counters();
    quiesce_result = ESP_FAIL;
    assert(app_spi_bus_register_display_guard(quiesce, resume, &callback_context) == ESP_OK);
    assert(app_spi_bus_sd_operation_begin() == ESP_FAIL);
    assert(quiesce_calls == 1U);
    assert(resume_calls == 0U);
    assert(mutex_take_calls == 2U); /* register + failed operation begin */
    assert(mutex_give_calls == 2U); /* registration + failed begin */
    app_spi_bus_unregister_display_guard();
}

static void test_busy_bus_fails_the_operation_instead_of_hanging(void)
{
    reset_counters();
    assert(app_spi_bus_register_display_guard(quiesce, resume, &callback_context) == ESP_OK);

    /* The SD path waits a bounded time, never for ever. */
    assert(app_spi_bus_sd_operation_begin() == ESP_OK);
    assert(last_mutex_timeout != portMAX_DELAY);
    assert(last_mutex_timeout == pdMS_TO_TICKS(APP_SPI_BUS_SD_MUTEX_WAIT_MS));
    app_spi_bus_sd_operation_end();

    /* The holder never lets go: the operation fails without touching the display or the mutex. */
    const unsigned gives_before = mutex_give_calls;
    const unsigned quiesce_before = quiesce_calls;
    mutex_take_result = pdFALSE;
    assert(app_spi_bus_sd_operation_begin() == ESP_ERR_TIMEOUT);
    assert(quiesce_calls == quiesce_before);
    assert(resume_calls == 1U);
    assert(mutex_give_calls == gives_before);
    mutex_take_result = pdTRUE;

    /* The bus works again once it is free. */
    assert(app_spi_bus_sd_operation_begin() == ESP_OK);
    app_spi_bus_sd_operation_end();
    assert(quiesce_calls == quiesce_before + 1U);
    assert(resume_calls == 2U);
    app_spi_bus_unregister_display_guard();
}

static void test_timing_lines_are_printed_only_when_they_say_something(void)
{
    const int64_t second = 1000000LL;
    app_spi_bus_timing_log_t log = {0};
    app_spi_bus_timing_summary_t summary = {0};
    bool summary_due = false;

    /* The first operation is the first sample: begin and end are printed, with an empty summary. */
    assert(app_spi_bus_timing_begin(&log, 10 * second, 5, 30, false));
    assert(app_spi_bus_timing_end(&log, 10 * second + 4000, 40, 4000, &summary_due, &summary));
    assert(summary_due);
    assert(summary.operations == 1U && summary.failures == 0U);
    assert(summary.window_us == 0);
    assert(summary.max_mutex_wait_us == 5 && summary.max_quiesce_us == 30);
    assert(summary.max_resume_us == 40 && summary.max_elapsed_us == 4000);

    /* Ordinary operations inside the period print nothing: this is the recorder's write path. */
    for (int op = 1; op <= 20; ++op) {
        const int64_t now = 10 * second + op * 100000LL;
        assert(!app_spi_bus_timing_begin(&log, now, 6 + op, 300, false));
        summary_due = true;
        assert(!app_spi_bus_timing_end(&log, now + 9000, 50, 9000, &summary_due, &summary));
        assert(!summary_due);
    }

    /* A long wait for the display is printed at once, but does not restart the summary window. */
    assert(app_spi_bus_timing_begin(&log, 13 * second, 7, APP_SPI_BUS_TIMING_NOTABLE_WAIT_US, false));
    assert(!app_spi_bus_timing_end(&log, 13 * second + 9000, 50, 9000, &summary_due, &summary));
    assert(!summary_due);
    assert(!app_spi_bus_timing_begin(&log, 13 * second + 20000, 7, APP_SPI_BUS_TIMING_NOTABLE_WAIT_US - 1, false));
    /* A slow operation or a slow display resume is printed at its end. */
    assert(app_spi_bus_timing_end(
        &log, 13 * second + 400000, 50, APP_SPI_BUS_TIMING_NOTABLE_ELAPSED_US, &summary_due, &summary));
    assert(!summary_due);
    assert(!app_spi_bus_timing_begin(&log, 14 * second, 7, 300, false));
    assert(app_spi_bus_timing_end(
        &log, 14 * second + 60000, APP_SPI_BUS_TIMING_NOTABLE_WAIT_US, 60000, &summary_due, &summary));
    assert(!summary_due);

    /* A failed begin is always printed, is counted, and is never the sampled operation. */
    assert(app_spi_bus_timing_begin(&log, 16 * second, 9, 0, true));
    assert(!log.sampled_operation);

    /* One period after the previous sample the next operation is printed with the summary
     * of everything in between: 20 quiet, 3 printed, 1 failed, 1 just short of the period, and itself. */
    assert(!app_spi_bus_timing_begin(&log, 10 * second + 4000 + APP_SPI_BUS_TIMING_SUMMARY_PERIOD_US - 1, 1, 1, false));
    assert(!app_spi_bus_timing_end(&log, 15 * second + 4000, 1, 1, &summary_due, &summary));
    assert(app_spi_bus_timing_begin(&log, 10 * second + 4000 + APP_SPI_BUS_TIMING_SUMMARY_PERIOD_US, 2, 2, false));
    assert(app_spi_bus_timing_end(&log, 15 * second + 9000, 3, 5000, &summary_due, &summary));
    assert(summary_due);
    assert(summary.operations == 26U);
    assert(summary.failures == 1U);
    assert(summary.window_us == 5 * second + 5000);
    assert(summary.max_mutex_wait_us == 26);
    assert(summary.max_quiesce_us == APP_SPI_BUS_TIMING_NOTABLE_WAIT_US);
    assert(summary.max_resume_us == APP_SPI_BUS_TIMING_NOTABLE_WAIT_US);
    assert(summary.max_elapsed_us == APP_SPI_BUS_TIMING_NOTABLE_ELAPSED_US);

    /* The window starts again from zero. */
    assert(!app_spi_bus_timing_begin(&log, 15 * second + 10000, 1, 1, false));
    assert(log.operations == 1U && log.failures == 0U && log.max_quiesce_us == 1);

    /* Without state every line is printed, as before. */
    assert(app_spi_bus_timing_begin(NULL, 0, 0, 0, false));
    assert(app_spi_bus_timing_end(NULL, 0, 0, 0, &summary_due, &summary));
    assert(!summary_due);
}

int main(void)
{
    test_init_sets_safe_levels_and_bus_config();
    test_guard_pairs_quiesce_and_resume();
    test_quiesce_failure_releases_mutex_without_resume();
    test_busy_bus_fails_the_operation_instead_of_hanging();
    test_timing_lines_are_printed_only_when_they_say_something();
    return 0;
}
