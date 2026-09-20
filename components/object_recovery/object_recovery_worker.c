#include "object_recovery.h"

#include "object_recovery_internal.h"

#include "app_task_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#ifdef ESP_PLATFORM
#include "esp_task_wdt.h"
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"


static const char *TAG = "OBJECT_RECOVERY";

#define OBJECT_RECOVERY_BACKGROUND_START_DELAY_MS   100U
#define OBJECT_RECOVERY_BACKGROUND_RESCAN_DELAY_MS  250U
#define OBJECT_RECOVERY_STACK_HWM_TARGET_BYTES      1024U

static TaskHandle_t s_task = NULL;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_starting = false;
static bool s_finishing = false;
static bool s_stop_requested = false;
static bool s_rescan_requested = false;
static object_recovery_background_complete_fn s_complete_fn = NULL;
static void *s_complete_context = NULL;


static void object_recovery_log_stack_hwm(void)
{
    const UBaseType_t free_bytes =
        uxTaskGetStackHighWaterMark(NULL);

    if (free_bytes < OBJECT_RECOVERY_STACK_HWM_TARGET_BYTES) {
        ESP_LOGW(
            TAG,
            "STACK_HWM task=object_recovery free=%u target=%u",
            (unsigned)free_bytes,
            (unsigned)OBJECT_RECOVERY_STACK_HWM_TARGET_BYTES
        );
    } else {
        ESP_LOGI(
            TAG,
            "STACK_HWM task=object_recovery free=%u target=%u",
            (unsigned)free_bytes,
            (unsigned)OBJECT_RECOVERY_STACK_HWM_TARGET_BYTES
        );
    }
}


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


static bool object_recovery_watchdog_begin(void)
{
#ifdef ESP_PLATFORM
    const esp_err_t err =
        esp_task_wdt_add(NULL);

    if (err == ESP_OK) {
        return true;
    }

    ESP_LOGW(
        TAG,
        "TWDT subscribe failed: %s",
        esp_err_to_name(err)
    );
#endif

    return false;
}


static void object_recovery_watchdog_end(
    bool subscribed
)
{
#ifdef ESP_PLATFORM
    if (!subscribed) {
        return;
    }

    const esp_err_t err =
        esp_task_wdt_delete(NULL);

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "TWDT unsubscribe failed: %s",
            esp_err_to_name(err)
        );
    }
#else
    (void)subscribed;
#endif
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

    /*
     * The background worker is short-lived, so watch it only while recovery
     * is actually active. object_recovery_run_cooperative() resets the TWDT at
     * completed directory/object/session progress boundaries; a blocked
     * filesystem call therefore cannot feed the watchdog.
     */
    const bool watchdog_subscribed =
        object_recovery_watchdog_begin();

    esp_err_t err = ESP_OK;
    unsigned pass = 0U;

    while (!object_recovery_background_stop_requested(NULL)) {
        portENTER_CRITICAL(&s_state_lock);
        s_rescan_requested = false;
        portEXIT_CRITICAL(&s_state_lock);

        ++pass;
        ESP_LOGI(
            TAG,
            "OBJECT_RECOVERY_BACKGROUND state=PASS pass=%u",
            pass
        );

        err = object_recovery_run_cooperative(
            object_recovery_background_stop_requested,
            NULL,
            true
        );
        if (err != ESP_OK || object_recovery_background_stop_requested(NULL)) {
            break;
        }

        bool rescan = false;
        portENTER_CRITICAL(&s_state_lock);
        rescan = s_rescan_requested;
        if (!rescan) {
            /* Seal this worker before leaving the pass loop. Any requester that
             * arrives from this point until s_task is retired waits for the
             * completion callback instead of attaching work to a pass that can
             * no longer observe s_rescan_requested. */
            s_finishing = true;
        }
        portEXIT_CRITICAL(&s_state_lock);

        if (!rescan) {
            break;
        }

        /* A failed final metadata commit may request recovery while the boot
         * archive pass is already active. Coalesce all such requests into one
         * next pass and insert a delay so repeated external failures can never
         * turn this worker into a storage hot loop. */
        ESP_LOGI(
            TAG,
            "OBJECT_RECOVERY_BACKGROUND state=RESCAN pass=%u",
            pass + 1U
        );
        vTaskDelay(
            pdMS_TO_TICKS(
                OBJECT_RECOVERY_BACKGROUND_RESCAN_DELAY_MS
            )
        );
    }

    object_recovery_watchdog_end(
        watchdog_subscribed
    );

    ESP_LOGI(
        TAG,
        "OBJECT_RECOVERY_BACKGROUND state=STOP result=%d passes=%u",
        (int)err,
        pass
    );

    object_recovery_background_complete_fn complete_fn = NULL;
    void *complete_context = NULL;
    bool stopped = false;

    portENTER_CRITICAL(&s_state_lock);
    /* Error/stop exits did not pass through the normal no-rescan seal above. */
    s_finishing = true;
    stopped = s_stop_requested;
    complete_fn = s_complete_fn;
    complete_context = s_complete_context;
    s_complete_fn = NULL;
    s_complete_context = NULL;
    s_rescan_requested = false;
    portEXIT_CRITICAL(&s_state_lock);

    /* Keep s_task non-NULL and s_finishing=true while the completion callback
     * executes. A concurrent recovery request waits for this bounded callback,
     * then creates a successor worker; it can never be lost in worker teardown.
     * Shutdown likewise waits until callback code is no longer using Web or
     * storage services. */
    if (!stopped && complete_fn != NULL) {
        complete_fn(err, complete_context);
    }

    /*
     * Final self-HWM sample, taken after the completion callback and before
     * task retirement so this short-lived worker cannot evade the 10 s
     * periodic runtime-observability sampler. Emitted on every exit path
     * (normal completion, error, or stop-requested).
     */
    object_recovery_log_stack_hwm();

    portENTER_CRITICAL(&s_state_lock);
    s_task = NULL;
    s_finishing = false;
    portEXIT_CRITICAL(&s_state_lock);

    vTaskDeleteWithCaps(NULL);
}


esp_err_t object_recovery_start_background_with_callback(
    object_recovery_background_complete_fn complete_fn,
    void *user_context
)
{
    for (;;) {
        portENTER_CRITICAL(&s_state_lock);

        if (s_stop_requested && (s_task != NULL || s_starting)) {
            portEXIT_CRITICAL(&s_state_lock);
            return ESP_ERR_INVALID_STATE;
        }

        if (s_task != NULL) {
            if (!s_finishing) {
                /* Do not create a competing archive walker. Remember one
                 * coalesced rescan request; the active worker clears the flag
                 * only when a new pass actually starts. */
                s_rescan_requested = true;
                if (complete_fn != NULL) {
                    s_complete_fn = complete_fn;
                    s_complete_context = user_context;
                }
                portEXIT_CRITICAL(&s_state_lock);
                ESP_LOGI(TAG, "OBJECT_RECOVERY_BACKGROUND state=RESCAN_QUEUED");
                return ESP_OK;
            }

            /* The current pass has atomically sealed itself and is executing
             * only bounded completion work. Block this higher-priority caller
             * for one scheduler tick so the low-priority recovery task can
             * retire, then retry creation as a successor worker. */
            portEXIT_CRITICAL(&s_state_lock);
            vTaskDelay(1);
            continue;
        }

        if (s_starting) {
            portEXIT_CRITICAL(&s_state_lock);
            vTaskDelay(1);
            continue;
        }

        /* Reserve creation under the same lock so simultaneous callers cannot
         * both observe s_task==NULL and create competing archive walkers. */
        s_starting = true;
        s_stop_requested = false;
        s_rescan_requested = false;
        s_finishing = false;
        s_complete_fn = complete_fn;
        s_complete_context = user_context;
        portEXIT_CRITICAL(&s_state_lock);
        break;
    }

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

    portENTER_CRITICAL(&s_state_lock);
    s_starting = false;
    if (result != pdPASS) {
        s_task = NULL;
        s_stop_requested = false;
        s_rescan_requested = false;
        s_finishing = false;
        s_complete_fn = NULL;
        s_complete_context = NULL;
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (result != pdPASS) {
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
    s_rescan_requested = false;
    const bool work_exists = s_task != NULL || s_starting;
    portEXIT_CRITICAL(&s_state_lock);

    if (!work_exists) {
        return;
    }

    ESP_LOGI(TAG, "OBJECT_RECOVERY_BACKGROUND state=STOP_REQUESTED");

    while (true) {
        portENTER_CRITICAL(&s_state_lock);
        const bool running = s_task != NULL || s_starting;
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
    const bool running = s_task != NULL || s_starting;
    portEXIT_CRITICAL(&s_state_lock);
    return running;
}
