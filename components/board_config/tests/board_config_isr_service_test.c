#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

#include "board_config.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


static unsigned s_install_calls;
static int s_install_flags;
static esp_err_t s_install_result = ESP_OK;
static int s_install_core = -1;

static int s_current_core;
static int s_caller_token;
static unsigned s_task_creates;
static int s_task_core = -1;
static BaseType_t s_task_create_result = pdPASS;
static bool s_task_runs = true;
static unsigned s_notifications;
static TaskHandle_t s_notified_task;
static unsigned s_task_deletes;
static TickType_t s_wait_ticks;


esp_err_t gpio_install_isr_service(int flags)
{
    ++s_install_calls;
    s_install_flags = flags;
    s_install_core = s_current_core;   /* IDF binds the service to the calling core */
    return s_install_result;
}

int xPortGetCoreID(void)
{
    return s_current_core;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return &s_caller_token;
}

BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t function,
    const char *name,
    uint32_t stack_bytes,
    void *argument,
    UBaseType_t priority,
    TaskHandle_t *handle,
    BaseType_t core
)
{
    (void)name;
    (void)priority;
    (void)handle;
    ++s_task_creates;
    s_task_core = (int)core;
    assert(stack_bytes >= 2048U);
    if (s_task_create_result != pdPASS) {
        return s_task_create_result;
    }
    if (s_task_runs) {
        /* The pinned task runs on its own core while the caller waits. */
        const int caller_core = s_current_core;
        s_current_core = (int)core;
        function(argument);
        s_current_core = caller_core;
    }
    return pdPASS;
}

BaseType_t xTaskNotifyGive(TaskHandle_t task)
{
    ++s_notifications;
    s_notified_task = task;
    return pdPASS;
}

uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t ticks_to_wait)
{
    assert(clear_on_exit == pdTRUE);
    s_wait_ticks = ticks_to_wait;
    const uint32_t pending = s_notifications;
    s_notifications = 0U;
    return pending;
}

void vTaskDelete(TaskHandle_t task)
{
    assert(task == NULL);
    ++s_task_deletes;
}


int main(void)
{
    /* app_main runs on core 0; the GPIO interrupt must still end up on the realtime core. */
    assert(BOARD_GPIO_ISR_CORE == 1);
    s_current_core = 0;

    /* No memory for the helper task: reported, nothing installed on the wrong core. */
    s_task_create_result = pdFAIL;
    assert(board_gpio_isr_service_install() == ESP_ERR_NO_MEM);
    assert(s_task_creates == 1U);
    assert(s_install_calls == 0U);
    s_task_create_result = pdPASS;

    /* The helper never reports: a bounded wait, then an error instead of a hang. */
    s_task_runs = false;
    assert(board_gpio_isr_service_install() == ESP_ERR_TIMEOUT);
    assert(s_task_creates == 2U);
    assert(s_install_calls == 0U);
    assert(s_wait_ticks > 0U && s_wait_ticks < 1000U);
    s_task_runs = true;

    /* The installer's own failure is passed through, and it ran on core 1, not on the caller's core. */
    s_install_result = ESP_FAIL;
    assert(board_gpio_isr_service_install() == ESP_FAIL);
    assert(s_task_creates == 3U);
    assert(s_task_core == BOARD_GPIO_ISR_CORE);
    assert(s_install_calls == 1U);
    assert(s_install_core == BOARD_GPIO_ISR_CORE);
    assert(s_install_flags == ESP_INTR_FLAG_IRAM);
    assert(s_notified_task == (TaskHandle_t)&s_caller_token);
    assert(s_task_deletes == 1U);
    assert(s_current_core == 0);

    /* Already on the realtime core: installed directly, no helper task.
     * A prior compatible installation is indistinguishable from any other
     * ESP_ERR_INVALID_STATE in IDF; the sole-owner contract documents this. */
    s_current_core = BOARD_GPIO_ISR_CORE;
    s_install_result = ESP_ERR_INVALID_STATE;
    assert(board_gpio_isr_service_install() == ESP_OK);
    assert(s_task_creates == 3U);
    assert(s_install_calls == 2U);
    assert(s_install_core == BOARD_GPIO_ISR_CORE);
    assert(s_install_flags == ESP_INTR_FLAG_IRAM);

    /* Installed once: later calls from any core do nothing. */
    s_current_core = 0;
    s_install_result = ESP_FAIL;
    assert(board_gpio_isr_service_install() == ESP_OK);
    assert(s_install_calls == 2U);
    assert(s_task_creates == 3U);
    return 0;
}
