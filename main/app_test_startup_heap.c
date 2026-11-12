#include "app_test_startup_heap.h"

#include <stddef.h>

#include "app_task_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TEST_HEAP_MAX_ALLOCS 64U
#define TEST_HEAP_LARGEST_MAX 3584U
/* 5455 bytes is the observed free heap; tolerate allocator and emulator layout. */
#define TEST_HEAP_FREE_MIN 4096U
#define TEST_HEAP_FREE_TARGET 5455U
#define TEST_HEAP_FREE_MAX 6144U
#define TEST_HEAP_ALLOC_MARGIN 128U

static const char *TAG = "TEST_STARTUP_HEAP";
static void *s_allocations[TEST_HEAP_MAX_ALLOCS];
static size_t s_allocation_count;

static void log_heap(const char *stage)
{
    const unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    ESP_LOGI(TAG, "%s free_internal=%u largest_internal=%u free_psram=%u", stage,
             (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void app_test_startup_heap_release(void)
{
    while (s_allocation_count > 0U) {
        heap_caps_free(s_allocations[--s_allocation_count]);
        s_allocations[s_allocation_count] = NULL;
    }
    log_heap("release");
}

static void control_task(void *argument)
{
    (void)argument;
    vTaskDelete(NULL);
}

bool app_test_startup_heap_prepare(void)
{
    const unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    if (s_allocation_count != 0U) {
        ESP_LOGE(TAG, "FAIL pressure already held");
        app_test_startup_heap_release();
        return false;
    }

    for (size_t attempt = 0U; attempt < TEST_HEAP_MAX_ALLOCS; ++attempt) {
        const size_t free_bytes = heap_caps_get_free_size(caps);
        const size_t largest = heap_caps_get_largest_free_block(caps);
        if (free_bytes >= TEST_HEAP_FREE_MIN && free_bytes <= TEST_HEAP_FREE_MAX &&
            largest <= TEST_HEAP_LARGEST_MAX) {
            log_heap("pressure_ready");
            const BaseType_t created = xTaskCreatePinnedToCore(
                control_task, "test_heap_control", 8192U, NULL, tskIDLE_PRIORITY + 1U,
                NULL, APP_TASK_CORE_SERVICES);
            if (created != pdPASS) {
                log_heap("control_rejected");
                const size_t after_free = heap_caps_get_free_size(caps);
                const size_t after_largest = heap_caps_get_largest_free_block(caps);
                if (after_free >= TEST_HEAP_FREE_MIN && after_free <= TEST_HEAP_FREE_MAX &&
                    after_largest <= TEST_HEAP_LARGEST_MAX) return true;
                ESP_LOGE(TAG, "FAIL pressure changed after control task");
                app_test_startup_heap_release();
                return false;
            }
            ESP_LOGE(TAG, "FAIL control task unexpectedly created");
            app_test_startup_heap_release();
            return false;
        }
        if (free_bytes <= TEST_HEAP_FREE_MIN + TEST_HEAP_ALLOC_MARGIN || largest <= TEST_HEAP_ALLOC_MARGIN) break;

        size_t amount = largest > TEST_HEAP_LARGEST_MAX
            ? largest - (TEST_HEAP_LARGEST_MAX - TEST_HEAP_ALLOC_MARGIN)
            : free_bytes - TEST_HEAP_FREE_TARGET;
        const size_t free_limit = free_bytes - TEST_HEAP_FREE_MIN - TEST_HEAP_ALLOC_MARGIN;
        const size_t largest_limit = largest - TEST_HEAP_ALLOC_MARGIN;
        if (amount > free_limit) amount = free_limit;
        if (amount > largest_limit) amount = largest_limit;
        if (amount == 0U) break;
        void *allocation = heap_caps_malloc(amount, caps);
        if (allocation == NULL) break;
        s_allocations[s_allocation_count++] = allocation;
    }
    log_heap("pressure_failed");
    ESP_LOGE(TAG, "FAIL could not establish bounded internal heap pressure");
    app_test_startup_heap_release();
    return false;
}
