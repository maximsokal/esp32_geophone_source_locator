#include "ranging_radio_pause.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_manager.h"

#define RADIO_PAUSE_GRACE_MS 3000U       /* the screen and the web page say "Wi-Fi turns off" this long before it does */
#define RADIO_PAUSE_RETRY_MS 5000U
#define RADIO_PAUSE_START_TRIES 12U      /* a minute of trying to bring the radio back */
#define RADIO_PAUSE_POLL_MS 100U
#define RADIO_PAUSE_STACK_BYTES 6144U    /* wifi_manager_stop() runs the web server's stop hooks in this task */
#define RADIO_PAUSE_PRIORITY 3U

static const char *TAG = "RADIO_PAUSE";

static ranging_radio_core_t s_core;
static volatile bool s_desired_off;
static volatile ranging_radio_state_t s_state;
static volatile bool s_stop_failed;
static volatile bool s_resume_failed;
static volatile bool s_running;

static bool op_running(void *context)
{
    (void)context;
    return wifi_manager_is_running();
}

static int op_stop(void *context)
{
    (void)context;
    const esp_err_t err = wifi_manager_stop();
    ESP_LOGW(TAG, "RADIO_PAUSE_STOP wifi_manager_stop=%s", esp_err_to_name(err));
    return err == ESP_OK ? 0 : -1;
}

static int op_start(void *context)
{
    (void)context;
    const esp_err_t err = wifi_manager_start();
    ESP_LOGW(TAG, "RADIO_PAUSE_START wifi_manager_start=%s", esp_err_to_name(err));
    return err == ESP_OK ? 0 : -1;
}

static uint64_t op_now_ms(void *context)
{
    (void)context;
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static void radio_pause_task(void *argument)
{
    (void)argument;
    for (;;) {
        const bool want = s_desired_off;
        if (want != s_core.want_off) {
            if (want) ranging_radio_core_request_off(&s_core);
            else ranging_radio_core_request_on(&s_core);
        }
        const ranging_radio_state_t before = s_core.state;
        ranging_radio_core_step(&s_core);
        s_state = s_core.state;
        s_stop_failed = s_core.stop_failed;
        s_resume_failed = s_core.resume_failed;
        if (s_core.state != before) {
            ESP_LOGI(TAG, "RADIO_PAUSE state=%s we_stopped=%u stop_failed=%u resume_failed=%u",
                ranging_radio_state_name(s_core.state), s_core.we_stopped ? 1U : 0U, s_core.stop_failed ? 1U : 0U,
                s_core.resume_failed ? 1U : 0U);
        }
        vTaskDelay(pdMS_TO_TICKS(RADIO_PAUSE_POLL_MS));
    }
}

esp_err_t ranging_radio_pause_init(void)
{
    if (s_running) return ESP_OK;
    const ranging_radio_ops_t ops = {op_running, op_stop, op_start, op_now_ms, NULL};
    ranging_radio_core_init(&s_core, &ops, RADIO_PAUSE_GRACE_MS, RADIO_PAUSE_RETRY_MS, RADIO_PAUSE_START_TRIES);
    s_desired_off = false;
    s_state = RANGING_RADIO_IDLE;
    if (xTaskCreate(radio_pause_task, "radio_pause", RADIO_PAUSE_STACK_BYTES, NULL, RADIO_PAUSE_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_running = true;
    return ESP_OK;
}

void ranging_radio_pause_request_off(void)
{
    if (s_running) s_desired_off = true;
}

void ranging_radio_pause_request_on(void)
{
    if (s_running) s_desired_off = false;
}

bool ranging_radio_pause_ready(void)
{
    return !s_running || s_state == RANGING_RADIO_OFF;
}

ranging_radio_state_t ranging_radio_pause_state(void)
{
    return s_running ? s_state : RANGING_RADIO_IDLE;
}

bool ranging_radio_pause_stop_failed(void)
{
    return s_running && s_stop_failed;
}

bool ranging_radio_pause_resume_failed(void)
{
    return s_running && s_resume_failed;
}
