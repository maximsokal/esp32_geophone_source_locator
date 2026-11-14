#include "app_test_recording.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "localization_job.h"
#include "measurement_service.h"
#include "nrf_topology_service.h"
#include "object_service.h"
#include "runtime_observability.h"
#include "sd_card.h"
#include "sdkconfig.h"
#include "workflow_service.h"

#define TEST_RECORDING_SETTLE_MS 12000U
#define TEST_RECORDING_RECORD_MS 10000U
#define TEST_RECORDING_POLL_MS 200U

/*
 * Board-equivalent internal DMA budget. On the board the internal DMA-capable heap is nearly used up once Wi-Fi, the web
 * server and the UI are up: the board log of 2026-11-14 10:29 measured 3647 bytes free with a 1856-byte largest block
 * when the recording started (static Wi-Fi buffers: 16 TX, 6 RX), and the SD driver then failed to get its DMA bounce
 * buffer. ESP-EMU has no TFT, no real Wi-Fi radio and no SDSPI, so its heap is nearly empty. The scenario therefore
 * holds ballast down to the level of the board, plus exactly what the configured static Wi-Fi buffers free compared to
 * the measured configuration: a configuration that gives the DMA heap back less than the recording needs fails S10.
 */
#define BOARD_START_DMA_FREE 3647U
#define BOARD_START_DMA_LARGEST 1856U
#define BOARD_WIFI_STATIC_TX_BUFFERS 16
#define BOARD_WIFI_STATIC_RX_BUFFERS 6
#define WIFI_STATIC_BUFFER_BYTES 1700U
#define TEST_DMA_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)
#define BALLAST_MAX_BLOCKS 160U
#define BOARD_PSRAM_FREE 3300000U
#define BOARD_PSRAM_LARGEST 3145728U

static const char *TAG = "TEST_RECORDING";

static void fail(const char *stage, esp_err_t err)
{
    ESP_LOGE(TAG, "FAIL stage=%s err=%s", stage, esp_err_to_name(err));
}

static void log_dma(const char *what)
{
    ESP_LOGI(TAG, "dma %s free=%u largest=%u internal_free=%u", what,
             (unsigned)heap_caps_get_free_size(TEST_DMA_CAPS), (unsigned)heap_caps_get_largest_free_block(TEST_DMA_CAPS),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

typedef struct {
    void *block[BALLAST_MAX_BLOCKS];
    size_t size[BALLAST_MAX_BLOCKS];
    size_t count;
} ballast_t;

static ballast_t s_dma_ballast;
static ballast_t s_psram_ballast;

static void ballast_release(ballast_t *ballast)
{
    while (ballast->count > 0U) heap_caps_free(ballast->block[--ballast->count]);
}

/*
 * Leave a heap in the shape of the board: take all of it, then give back two blocks, one of `largest_block` bytes and one
 * holding the rest of `target_free`. Returns true when the result is within `tolerance_percent` of both.
 */
static bool ballast_apply(ballast_t *ballast, uint32_t caps, size_t target_free, size_t largest_block, size_t tolerance_percent)
{
    while (ballast->count < BALLAST_MAX_BLOCKS) {
        const size_t largest = heap_caps_get_largest_free_block(caps);
        if (largest < 32U) break;
        void *block = heap_caps_malloc(largest, caps);
        if (block == NULL) break;
        ballast->size[ballast->count] = largest;
        ballast->block[ballast->count++] = block;
    }
    const size_t wanted[2] = {largest_block, target_free > largest_block ? target_free - largest_block : 0U};
    for (unsigned w = 0U; w < 2U; ++w) {
        /* A second free piece smaller than this would only merge with the first one: skip it. */
        if (wanted[w] < (w == 0U ? 64U : 262144U)) continue;
        /* The two blocks must come from different places, or the freed tails merge into one large block. */
        for (size_t step = 0U; step < ballast->count; ++step) {
            const size_t b = w == 0U ? step : ballast->count - 1U - step;
            if (ballast->size[b] < wanted[w] + 64U) continue;
            void *shrunk = heap_caps_realloc(ballast->block[b], ballast->size[b] - wanted[w], caps);
            if (shrunk != NULL) {
                ballast->block[b] = shrunk;
                ballast->size[b] -= wanted[w];
            }
            break;
        }
    }
    const size_t free_bytes = heap_caps_get_free_size(caps);
    const size_t largest = heap_caps_get_largest_free_block(caps);
    const size_t free_tolerance = 400U + target_free * tolerance_percent / 100U;
    const size_t largest_tolerance = 300U + largest_block * (tolerance_percent + 10U) / 100U;
    return free_bytes + free_tolerance >= target_free && free_bytes <= target_free + free_tolerance &&
           largest + largest_tolerance >= largest_block && largest <= largest_block + largest_tolerance;
}

static bool wait_state(measurement_service_state_t wanted, uint32_t timeout_ms, measurement_service_snapshot_t *out)
{
    for (uint32_t waited = 0U; waited <= timeout_ms; waited += TEST_RECORDING_POLL_MS) {
        if (measurement_service_get_snapshot(out) == ESP_OK) {
            if (out->state == wanted) return true;
            if (out->state == MEASUREMENT_SERVICE_ERROR) return false;
        }
        vTaskDelay(pdMS_TO_TICKS(TEST_RECORDING_POLL_MS));
    }
    return false;
}

static void scenario_task(void *argument)
{
    (void)argument;
    vTaskDelay(pdMS_TO_TICKS(TEST_RECORDING_SETTLE_MS));
    ESP_LOGI(TAG, "begin");

    const int freed_buffers = (BOARD_WIFI_STATIC_TX_BUFFERS - CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM) +
                              (BOARD_WIFI_STATIC_RX_BUFFERS - CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM);
    const int freed = freed_buffers * (int)WIFI_STATIC_BUFFER_BYTES;
    const size_t extra = freed > 0 ? (size_t)freed : 0U;
    const size_t target_free = (size_t)BOARD_START_DMA_FREE + extra;
    const size_t max_largest = (size_t)BOARD_START_DMA_LARGEST + extra;  /* the largest block the budget gives back */
    log_dma("before-ballast");
    const bool ballast_ok = ballast_apply(&s_dma_ballast, TEST_DMA_CAPS, target_free, max_largest, 10U);
    ESP_LOGI(TAG, "dma ballast tx=%d rx=%d freed=%d target_free=%u max_largest=%u applied=%d",
             CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM, CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM, freed,
             (unsigned)target_free, (unsigned)max_largest, ballast_ok ? 1 : 0);
    log_dma("board-equivalent");
    if (!ballast_ok) { fail("dma_ballast", ESP_ERR_NO_MEM); goto done; }

    /* The PSRAM the board has when it localizes: its largest free block is 3 MiB (board log 2026-11-14 16:02: 3145728). */
    const uint32_t psram_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    const bool psram_ok = ballast_apply(&s_psram_ballast, psram_caps, BOARD_PSRAM_FREE, BOARD_PSRAM_LARGEST, 5U);
    ESP_LOGI(TAG, "psram ballast target_free=%u largest=%u applied=%d free=%u largest_now=%u", (unsigned)BOARD_PSRAM_FREE,
             (unsigned)BOARD_PSRAM_LARGEST, psram_ok ? 1 : 0, (unsigned)heap_caps_get_free_size(psram_caps),
             (unsigned)heap_caps_get_largest_free_block(psram_caps));
    if (!psram_ok) { fail("psram_ballast", ESP_ERR_NO_MEM); goto done; }

    esp_err_t err = object_service_create_async();
    if (err != ESP_OK) { fail("create_object", err); goto done; }
    object_service_summary_t object = {0};
    bool have_object = false;
    for (uint32_t waited = 0U; waited < 20000U; waited += TEST_RECORDING_POLL_MS) {
        if (object_service_get_active(&object) == ESP_OK && object.active && object.id[0] != '\0') {
            have_object = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(TEST_RECORDING_POLL_MS));
    }
    if (!have_object) { fail("active_object", ESP_ERR_TIMEOUT); goto done; }
    ESP_LOGI(TAG, "object=%s", object.id);

    err = nrf_topology_service_test_seed_confirmed_setup(object.id);
    if (err != ESP_OK) { fail("seed_setup", err); goto done; }
    ESP_LOGI(TAG, "setup seeded");

    err = measurement_service_start_async();
    if (err != ESP_OK) { fail("start_async", err); goto done; }
    measurement_service_snapshot_t snapshot = {0};
    if (!wait_state(MEASUREMENT_SERVICE_RECORDING, 30000U, &snapshot)) {
        ESP_LOGE(TAG, "FAIL stage=recording state=%d last_error=%s", (int)snapshot.state,
                 esp_err_to_name(snapshot.last_error));
        goto done;
    }
    ESP_LOGI(TAG, "recording started session=%s", snapshot.session_id);

    vTaskDelay(pdMS_TO_TICKS(TEST_RECORDING_RECORD_MS));
    runtime_observability_request_snapshot("recording-scenario-peak");
    log_dma("recording");
    (void)measurement_service_get_snapshot(&snapshot);
    ESP_LOGI(TAG, "recording samples=%llu dropped=%llu ms=%llu", (unsigned long long)snapshot.samples_written,
             (unsigned long long)snapshot.dropped_frames, (unsigned long long)snapshot.recording_ms);

    err = measurement_service_stop_async();
    if (err != ESP_OK) { fail("stop_async", err); goto done; }
    if (!wait_state(MEASUREMENT_SERVICE_REVIEW, 60000U, &snapshot)) {
        ESP_LOGE(TAG, "FAIL stage=stop state=%d last_error=%s stop_error=%s", (int)snapshot.state,
                 esp_err_to_name(snapshot.last_error), esp_err_to_name(snapshot.stop_stage_error));
        goto done;
    }
    ESP_LOGI(TAG, "stopped state=REVIEW samples=%llu dropped=%llu", (unsigned long long)snapshot.samples_written,
             (unsigned long long)snapshot.dropped_frames);
    runtime_observability_request_snapshot("recording-scenario-end");

    /* The same calculation the user starts with CALCULATE_SOURCES, on the session just recorded. */
    uint32_t job_id = 0U;
    err = workflow_service_test_submit_calculation(snapshot.object_id, snapshot.session_id, &job_id);
    if (err != ESP_OK) { fail("calculation_submit", err); goto done; }
    localization_job_status_t job = {0};
    for (uint32_t waited = 0U; waited < 120000U; waited += TEST_RECORDING_POLL_MS) {
        localization_job_get_status(&job);
        if (job.job_id == job_id && (job.state == LOCALIZATION_JOB_SUCCEEDED || job.state == LOCALIZATION_JOB_FAILED)) break;
        vTaskDelay(pdMS_TO_TICKS(TEST_RECORDING_POLL_MS));
    }
    ESP_LOGI(TAG, "calculation job=%u state=%s solved=%u attempted=%u total=%u skipped=%u elapsed_ms=%u error=%s",
             (unsigned)job.job_id, localization_job_state_name(job.state), (unsigned)job.summary.methods_solved,
             (unsigned)job.summary.methods_attempted, (unsigned)job.summary.methods_total,
             (unsigned)job.summary.methods_skipped_for_time, (unsigned)job.elapsed_ms, esp_err_to_name(job.error));
    vTaskDelay(pdMS_TO_TICKS(1500U));
    ESP_LOGI(TAG, "sd dma bounce failures=%u", sd_card_test_flash_bounce_failures());
    ESP_LOGI(TAG, "DONE");

done:
    ballast_release(&s_dma_ballast);
    ballast_release(&s_psram_ballast);
    vTaskDeleteWithCaps(NULL);
}

void app_test_recording_scenario_start(void)
{
    const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        scenario_task, "test_recording", 6144U, NULL, tskIDLE_PRIORITY + 2U, NULL, 0,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) ESP_LOGE(TAG, "FAIL stage=task_create err=ESP_ERR_NO_MEM");
}
