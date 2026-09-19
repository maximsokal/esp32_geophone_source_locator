#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "spi_bus.h"

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
    (void)timeout;
    ++mutex_take_calls;
    return pdTRUE;
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

int main(void)
{
    test_init_sets_safe_levels_and_bus_config();
    test_guard_pairs_quiesce_and_resume();
    test_quiesce_failure_releases_mutex_without_resume();
    return 0;
}
