#include "object_recovery.h"

#include "object_recovery_internal.h"

#include "app_task_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"


static const char *TAG = "OBJECT_RECOVERY";

#define OBJECT_RECOVERY_BACKGROUND_START_DELAY_MS  100U

static TaskHandle_t s_task = NULL;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_stop_requested = false;
static object_recovery_background_complete_fn s_complete_fn = NULL;
static void *s_complete_context = NULL;


static bool object_recovery_background_stop_requested(
    void *user_context
)
{
    (void)user_context;

    bool requested = false;
    portENTER_CRITICAL(&s_state_lock);
    requested = s_stop_requested;
    portEXIT_CRITICAL(&s_state_lock);
    return requested;
}


static void object_recovery_background_task(void *argument)
{
    (void)argument;

    /* Give app_main enough time to finish the interactive startup path and
     * enter its shutdown wait before low-priority SD housekeeping begins. */
    vTaskDelay(
        pdMS_TO_TICKS(
            OBJECT_RECOVERY_BACKGROUND_START_DELAY_MS
        )
    );

    ESP_LOGI(
        TAG,
        "OBJECT_RECOVERY_BACKGROUND state=START core=%d priority=%u",
        APP_TASK_CORE_SERVICES,
        (unsigned)APP_TASK_PRIORITY_OBJECT_RECOVERY
    );

    esp_err_t err = ESP_OK;
    if (!object_recovery_background_stop_requested(NULL)) {
        err = object_recovery_run_cooperative(
            object_recovery_background_stop_requested,
            NULL,
            true
        );
    }

    ESP_LOGI(
        TAG,
        "OBJECT_RECOVERY_BACKGROUND state=STOP result=%d",
        (int)err
    );

    object_recovery_background_complete_fn complete_fn = NULL;
    void *complete_context = NULL;
    bool stopped = false;

    portENTER_CRITICAL(&s_state_lock);
    stopped = s_stop_requested;
    complete_fn = s_complete_fn;
    complete_context = s_complete_context;
    s_complete_fn = NULL;
    s_complete_context = NULL;
    portEXIT_CRITICAL(&s_state_lock);

    /* Keep s_task non-NULL while the completion callback executes. A
     * concurrent shutdown therefore waits in object_recovery_stop_background()
     * until the callback has returned and cannot tear web/storage services
     * down underneath it. */
    if (!stopped && complete_fn != NULL) {
        complete_fn(err, complete_context);
    }

    portENTER_CRITICAL(&s_state_lock);
    s_task = NULL;
    portEXIT_CRITICAL(&s_state_lock);

    vTaskDeleteWithCaps(NULL);
}


esp_err_t object_recovery_start_background_with_callback(
    object_recovery_background_complete_fn complete_fn,
    void *user_context
)
{
    portENTER_CRITICAL(&s_state_lock);
    if (s_task != NULL) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_OK;
    }
    s_stop_requested = false;
    s_complete_fn = complete_fn;
    s_complete_context = user_context;
    portEXIT_CRITICAL(&s_state_lock);

    const BaseType_t result =
        xTaskCreatePinnedToCoreWithCaps(
            object_recovery_background_task,
            "object_recovery",
            APP_TASK_STACK_OBJECT_RECOVERY,
            NULL,
            APP_TASK_PRIORITY_OBJECT_RECOVERY,
            &s_task,
            APP_TASK_CORE_SERVICES,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );

    if (result != pdPASS) {
        portENTER_CRITICAL(&s_state_lock);
        s_task = NULL;
        s_stop_requested = false;
        s_complete_fn = NULL;
        s_complete_context = NULL;
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "OBJECT_RECOVERY_BACKGROUND state=QUEUED stack=%u stack_caps=spiram|8bit",
        (unsigned)APP_TASK_STACK_OBJECT_RECOVERY
    );
    return ESP_OK;
}


esp_err_t object_recovery_start_background(void)
{
    return object_recovery_start_background_with_callback(
        NULL,
        NULL
    );
}


void object_recovery_stop_background(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_stop_requested = true;
    TaskHandle_t task = s_task;
    portEXIT_CRITICAL(&s_state_lock);

    if (task == NULL) {
        return;
    }

    ESP_LOGI(TAG, "OBJECT_RECOVERY_BACKGROUND state=STOP_REQUESTED");

    while (true) {
        portENTER_CRITICAL(&s_state_lock);
        const bool running = s_task != NULL;
        portEXIT_CRITICAL(&s_state_lock);

        if (!running) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}


bool object_recovery_is_running(void)
{
    portENTER_CRITICAL(&s_state_lock);
    const bool running = s_task != NULL;
    portEXIT_CRITICAL(&s_state_lock);
    return running;
}
