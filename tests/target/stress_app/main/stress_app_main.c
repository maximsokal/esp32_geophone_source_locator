#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "seismic_buffer.h"
#include "seismic_recorder_cache.h"
#include "stress_physical_probe.h"


#define STRESS_SEED                         0x84AE1732U
#define STRESS_RECORDER_PERIOD_MS                 10U
#define STRESS_RECORDER_BATCH_FRAMES             256U
#define STRESS_TRACE_CAPACITY                    64U
#define STRESS_PRESSURE_DMA_BYTES              4096U
#define STRESS_PRESSURE_PSRAM_BYTES           65536U
#define STRESS_RATE_DURATION_MS                1000U
#define STRESS_BINARY_DURATION_MS              400U
#define STRESS_SATURATION_DURATION_MS        1500U
#define STRESS_RATE_MAX_HZ                    5000U
#define STRESS_SCHEDULE_EVENT_COUNT             64U
#define STRESS_SCHEDULE_ISR_EVENT              255U


typedef struct
{
    uint32_t sequence;
    uint64_t timestamp_us;
    uint32_t event;
    uint32_t jitter_us;

} stress_trace_entry_t;


typedef struct
{
    uint32_t source_order;
    uint32_t event;
    uint32_t release_us;
    uint32_t jitter_us;

} stress_schedule_event_t;


typedef struct
{
    const char *name;
    uint32_t rate_hz;
    uint32_t duration_ms;
    uint32_t sd_delay_ms;
    size_t cache_capacity_frames;
    uint32_t seed;
    bool expect_saturated;

    seismic_recorder_cache_t cache;
    seismic_recorder_cache_slot_t *cache_storage;
    seismic_frame_t *recorder_batch;
    uint64_t recorder_cursor;
    volatile bool running;
    volatile uint64_t produced_frames;
    volatile uint64_t recorder_frames;
    volatile uint64_t ring_dropped_frames;
    volatile uint64_t cache_overflow_frames;
    volatile uint64_t producer_deadline_misses;
    volatile uint64_t recorder_errors;
    volatile uint64_t ui_ticks;
    volatile uint64_t sensor_ticks;
    volatile uint64_t network_ticks;
    volatile uint64_t isr_ticks;
    volatile uint32_t trace_write_index;
    uint32_t sensor_random_state;
    uint32_t network_random_state;
    uint32_t scheduler_random_state;
    portMUX_TYPE trace_lock;
    stress_trace_entry_t trace[STRESS_TRACE_CAPACITY];
    TaskHandle_t producer_task;
    TaskHandle_t recorder_task;
    TaskHandle_t ui_task;
    TaskHandle_t sensor_task;
    TaskHandle_t network_task;
    TaskHandle_t scheduler_task;
    esp_timer_handle_t isr_timer;
    uint32_t producer_stack_free_bytes;
    uint32_t recorder_stack_free_bytes;
    uint32_t ui_stack_free_bytes;
    uint32_t sensor_stack_free_bytes;
    uint32_t network_stack_free_bytes;
    uint32_t scheduler_stack_free_bytes;
    size_t internal_free_start;
    size_t internal_free_end;
    size_t dma_free_start;
    size_t dma_free_end;
    size_t psram_free_start;
    size_t psram_free_end;
    bool cache_in_psram;
    bool recorder_batch_in_psram;
    bool isr_timer_started;
    bool task_create_failed;

} stress_phase_t;


static uint32_t stress_random(uint32_t *state)
{
    uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}


static uint32_t stress_schedule_random(uint32_t *state)
{
    uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}


static void stress_make_schedule(
    uint32_t seed,
    stress_schedule_event_t events[STRESS_SCHEDULE_EVENT_COUNT]
)
{
    uint32_t random_state = seed;
    for (uint32_t index = 0U; index < STRESS_SCHEDULE_EVENT_COUNT; ++index) {
        events[index] = (stress_schedule_event_t){
            .source_order = index,
            .event = index % 7U == 0U ? STRESS_SCHEDULE_ISR_EVENT : index % 6U,
            .release_us = index * 1000U + stress_schedule_random(&random_state) % 250U,
            .jitter_us = stress_schedule_random(&random_state) % 500U,
        };
    }

    for (size_t index = STRESS_SCHEDULE_EVENT_COUNT - 1U; index > 0U; --index) {
        const size_t other = stress_schedule_random(&random_state) % (index + 1U);
        const stress_schedule_event_t temporary = events[index];
        events[index] = events[other];
        events[other] = temporary;
    }
}


static uint32_t stress_schedule_hash(
    const stress_schedule_event_t events[STRESS_SCHEDULE_EVENT_COUNT]
)
{
    uint32_t hash = 2166136261U;
    for (size_t index = 0U; index < STRESS_SCHEDULE_EVENT_COUNT; ++index) {
        const uint32_t values[] = {
            events[index].source_order,
            events[index].event,
            events[index].release_us,
            events[index].jitter_us,
        };
        for (size_t value_index = 0U; value_index < 4U; ++value_index) {
            hash ^= values[value_index];
            hash *= 16777619U;
        }
    }
    return hash;
}


static size_t stress_schedule_inversions(
    const stress_schedule_event_t events[STRESS_SCHEDULE_EVENT_COUNT]
)
{
    size_t inversions = 0U;
    for (size_t left = 0U; left < STRESS_SCHEDULE_EVENT_COUNT; ++left) {
        for (size_t right = left + 1U; right < STRESS_SCHEDULE_EVENT_COUNT; ++right) {
            if (events[left].source_order > events[right].source_order) {
                inversions++;
            }
        }
    }
    return inversions;
}


static bool stress_schedule_probe(uint32_t seed)
{
    static stress_schedule_event_t first[STRESS_SCHEDULE_EVENT_COUNT];
    static stress_schedule_event_t replay[STRESS_SCHEDULE_EVENT_COUNT];
    static stress_schedule_event_t other[STRESS_SCHEDULE_EVENT_COUNT];
    stress_make_schedule(seed, first);
    stress_make_schedule(seed, replay);
    stress_make_schedule(seed + 1U, other);

    const uint32_t first_hash = stress_schedule_hash(first);
    const size_t inversions = stress_schedule_inversions(first);
    const bool replay_ok =
        memcmp(first, replay, sizeof(first)) == 0 &&
        first_hash == stress_schedule_hash(replay) &&
        first_hash != stress_schedule_hash(other) &&
        inversions > 0U;
    printf(
        "TARGET_STRESS_SCHEDULE seed=%u events=%u inversions=%u "
        "trace_hash=%u replay=%s\n",
        (unsigned)seed,
        (unsigned)STRESS_SCHEDULE_EVENT_COUNT,
        (unsigned)inversions,
        (unsigned)first_hash,
        replay_ok ? "PASS" : "FAIL"
    );
    return replay_ok;
}


static void stress_trace(
    stress_phase_t *phase,
    uint32_t event,
    uint32_t jitter_us
)
{
    portENTER_CRITICAL(&phase->trace_lock);
    const uint32_t sequence = phase->trace_write_index++;
    const size_t index = sequence % STRESS_TRACE_CAPACITY;
    phase->trace[index] = (stress_trace_entry_t){
        .sequence = sequence,
        .timestamp_us = esp_timer_get_time(),
        .event = event,
        .jitter_us = jitter_us,
    };
    portEXIT_CRITICAL(&phase->trace_lock);
}


static void stress_isr_callback(void *argument)
{
    stress_phase_t *phase = (stress_phase_t *)argument;
    phase->isr_ticks++;
    /* ISR callbacks do not print or call heap APIs. The event is represented
     * in the same deterministic trace through the task-side counter. */
}


static void log_stack_hwm(
    const char *task_name,
    TaskHandle_t task,
    uint32_t *destination
)
{
    const UBaseType_t free_bytes = uxTaskGetStackHighWaterMark(task);
    *destination = (uint32_t)free_bytes;
    printf(
        "TARGET_STRESS_STACK task=%s core=%d free_bytes=%u\n",
        task_name,
        xPortGetCoreID(),
        (unsigned)free_bytes
    );
}


static void producer_task(void *argument)
{
    stress_phase_t *phase = (stress_phase_t *)argument;
    const uint64_t period_us = 1000000ULL / phase->rate_hz;
    uint64_t next_sample_us = (uint64_t)esp_timer_get_time();
    uint64_t frame_number = 0U;

    while (phase->running) {
        const uint64_t now_us = (uint64_t)esp_timer_get_time();
        if (now_us < next_sample_us) {
            taskYIELD();
            continue;
        }

        if (now_us > next_sample_us + period_us) {
            phase->producer_deadline_misses++;
        }

        float channels[SEISMIC_CHANNEL_COUNT] = {
            (float)frame_number,
            -(float)frame_number,
            (float)frame_number + 0.25f,
            (float)frame_number - 0.25f,
        };
        if (seismic_buffer_push((int64_t)now_us, channels) == ESP_OK) {
            phase->produced_frames++;
        } else {
            phase->recorder_errors++;
        }

        frame_number++;
        next_sample_us += period_us;
        if (now_us > next_sample_us + period_us * 4ULL) {
            next_sample_us = now_us + period_us;
        }
    }

    log_stack_hwm("producer", NULL, &phase->producer_stack_free_bytes);
    phase->producer_task = NULL;
    vTaskDelete(NULL);
}


static void recorder_task(void *argument)
{
    stress_phase_t *phase = (stress_phase_t *)argument;
    TickType_t last_wake = xTaskGetTickCount();

    if (phase->recorder_batch == NULL) {
        phase->recorder_errors++;
        log_stack_hwm("recorder", NULL, &phase->recorder_stack_free_bytes);
        phase->recorder_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (phase->running) {
        uint64_t candidate_cursor = phase->recorder_cursor;
        size_t frame_count = 0U;
        uint64_t dropped_frames = 0U;
        seismic_buffer_info_t info = {0};
        const esp_err_t copy_err = seismic_buffer_copy_since(
            &candidate_cursor,
            phase->recorder_batch,
            STRESS_RECORDER_BATCH_FRAMES,
            &frame_count,
            &dropped_frames,
            &info
        );
        if (copy_err != ESP_OK) {
            phase->recorder_errors++;
        } else {
            phase->ring_dropped_frames += dropped_frames;
            if (frame_count > 0U) {
                const uint64_t first_source_frame =
                    candidate_cursor - (uint64_t)frame_count + 1ULL;
                const esp_err_t append_err = seismic_recorder_cache_append(
                    &phase->cache,
                    first_source_frame,
                    phase->recorder_batch,
                    frame_count
                );
                if (append_err == ESP_ERR_NO_MEM) {
                    phase->cache_overflow_frames += frame_count;
                } else if (append_err != ESP_OK) {
                    phase->recorder_errors++;
                } else {
                    phase->recorder_cursor = candidate_cursor;
                    phase->recorder_frames += frame_count;
                }
            }
        }

        seismic_recorder_cache_info_t cache_info = {0};
        if (seismic_recorder_cache_get_info(&phase->cache, &cache_info) == ESP_OK &&
            cache_info.dirty_frames >= STRESS_RECORDER_BATCH_FRAMES) {
            size_t dirty_count = 0U;
            uint64_t first_session_frame = 0U;
            if (seismic_recorder_cache_copy_dirty(
                    &phase->cache,
                    phase->recorder_batch,
                    STRESS_RECORDER_BATCH_FRAMES,
                    &dirty_count,
                    &first_session_frame
                ) == ESP_OK && dirty_count > 0U) {
                vTaskDelay(pdMS_TO_TICKS(phase->sd_delay_ms));
                uint64_t last_source_frame = 0U;
                if (seismic_recorder_cache_mark_persisted(
                        &phase->cache,
                        first_session_frame,
                        dirty_count,
                        &last_source_frame
                    ) != ESP_OK) {
                    phase->recorder_errors++;
                }
            }
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(STRESS_RECORDER_PERIOD_MS));
    }

    log_stack_hwm("recorder", NULL, &phase->recorder_stack_free_bytes);
    phase->recorder_task = NULL;
    vTaskDelete(NULL);
}


static void ui_task(void *argument)
{
    stress_phase_t *phase = (stress_phase_t *)argument;
    TickType_t last_wake = xTaskGetTickCount();
    while (phase->running) {
        phase->ui_ticks++;
        stress_trace(phase, 1U, 0U);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(16U));
    }
    log_stack_hwm("ui", NULL, &phase->ui_stack_free_bytes);
    phase->ui_task = NULL;
    vTaskDelete(NULL);
}


static void sensor_task(void *argument)
{
    stress_phase_t *phase = (stress_phase_t *)argument;
    TickType_t last_wake = xTaskGetTickCount();
    while (phase->running) {
        void *dma = heap_caps_malloc(
            STRESS_PRESSURE_DMA_BYTES,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA
        );
        void *psram = heap_caps_malloc(
            STRESS_PRESSURE_PSRAM_BYTES,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
        phase->sensor_ticks++;
        stress_trace(phase, 2U, stress_random(&phase->sensor_random_state) % 80U);
        heap_caps_free(dma);
        heap_caps_free(psram);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(10U));
    }
    log_stack_hwm("sensor", NULL, &phase->sensor_stack_free_bytes);
    phase->sensor_task = NULL;
    vTaskDelete(NULL);
}


static void network_task(void *argument)
{
    stress_phase_t *phase = (stress_phase_t *)argument;
    TickType_t last_wake = xTaskGetTickCount();
    while (phase->running) {
        void *internal = heap_caps_malloc(
            2048U,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        );
        void *psram = heap_caps_malloc(
            8192U,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
        phase->network_ticks++;
        stress_trace(phase, 3U, stress_random(&phase->network_random_state) % 120U);
        heap_caps_free(internal);
        heap_caps_free(psram);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(25U));
    }
    log_stack_hwm("network", NULL, &phase->network_stack_free_bytes);
    phase->network_task = NULL;
    vTaskDelete(NULL);
}


static void scheduler_noise_task(void *argument)
{
    stress_phase_t *phase = (stress_phase_t *)argument;
    TickType_t last_wake = xTaskGetTickCount();
    while (phase->running) {
        const uint32_t jitter_us =
            stress_random(&phase->scheduler_random_state) % 150U;
        stress_trace(
            phase,
            4U + stress_random(&phase->scheduler_random_state) % 3U,
            jitter_us
        );
        esp_rom_delay_us(jitter_us);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(1U));
    }
    log_stack_hwm("scheduler", NULL, &phase->scheduler_stack_free_bytes);
    phase->scheduler_task = NULL;
    vTaskDelete(NULL);
}


static void log_heap_snapshot(
    stress_phase_t *phase,
    const char *stage,
    bool start
)
{
    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t dma_free = heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA
    );
    const size_t psram_free = heap_caps_get_free_size(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (start) {
        phase->internal_free_start = internal_free;
        phase->dma_free_start = dma_free;
        phase->psram_free_start = psram_free;
    } else {
        phase->internal_free_end = internal_free;
        phase->dma_free_end = dma_free;
    }
    printf(
        "TARGET_STRESS_HEAP stage=%s internal_free=%u dma_free=%u psram_free=%u "
        "internal_largest=%u dma_largest=%u psram_largest=%u\n",
        stage,
        (unsigned)internal_free,
        (unsigned)dma_free,
        (unsigned)psram_free,
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    );
}


static void dump_trace(stress_phase_t *phase)
{
    const uint32_t total = phase->trace_write_index;
    const uint32_t first = total > 16U ? total - 16U : 0U;
    for (uint32_t sequence = first; sequence < total; ++sequence) {
        const stress_trace_entry_t *entry =
            &phase->trace[sequence % STRESS_TRACE_CAPACITY];
        printf(
            "TARGET_STRESS_TRACE seq=%u event=%u t_us=%llu jitter_us=%u\n",
            (unsigned)entry->sequence,
            (unsigned)entry->event,
            (unsigned long long)entry->timestamp_us,
            (unsigned)entry->jitter_us
        );
    }
}


static bool phase_is_saturated(const stress_phase_t *phase)
{
    return phase->ring_dropped_frames > 0U ||
           phase->cache_overflow_frames > 0U ||
           phase->producer_deadline_misses > 0U ||
           phase->recorder_errors > 0U;
}


static bool run_phase(
    const char *name,
    uint32_t rate_hz,
    uint32_t duration_ms,
    uint32_t sd_delay_ms,
    size_t cache_capacity_frames,
    bool expected_saturated,
    uint32_t seed
)
{
    const bool schedule_ok = stress_schedule_probe(seed);
    stress_phase_t *phase = heap_caps_calloc(
        1U,
        sizeof(*phase),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    );
    if (phase == NULL) {
        printf(
            "TARGET_STRESS_SCENARIO name=%s rate_hz=%u status=FAIL reason=phase_alloc\n",
            name,
            (unsigned)rate_hz
        );
        return false;
    }
    *phase = (stress_phase_t){
        .name = name,
        .rate_hz = rate_hz,
        .duration_ms = duration_ms,
        .sd_delay_ms = sd_delay_ms,
        .cache_capacity_frames = cache_capacity_frames,
        .seed = seed,
        .expect_saturated = expected_saturated,
        .running = true,
        .sensor_random_state = seed ^ 0x13579BDFU,
        .network_random_state = seed ^ 0x2468ACE0U,
        .scheduler_random_state = seed ^ 0xA5A5A5A5U,
        .trace_lock = portMUX_INITIALIZER_UNLOCKED,
    };
    phase->cache_storage = heap_caps_malloc(
        cache_capacity_frames * sizeof(*phase->cache_storage),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    phase->cache_in_psram = phase->cache_storage != NULL;
    if (phase->cache_storage == NULL) {
        phase->cache_storage = heap_caps_malloc(
            cache_capacity_frames * sizeof(*phase->cache_storage),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        );
    }
    phase->recorder_batch = heap_caps_malloc(
        STRESS_RECORDER_BATCH_FRAMES * sizeof(*phase->recorder_batch),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    phase->recorder_batch_in_psram = phase->recorder_batch != NULL;
    if (phase->recorder_batch == NULL) {
        phase->recorder_batch = heap_caps_malloc(
            STRESS_RECORDER_BATCH_FRAMES * sizeof(*phase->recorder_batch),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        );
    }
    if (
        phase->cache_storage == NULL ||
        phase->recorder_batch == NULL ||
        seismic_recorder_cache_init(
            &phase->cache,
            phase->cache_storage,
            cache_capacity_frames
        ) != ESP_OK ||
        seismic_buffer_configure(SEISMIC_SOURCE_TEST, rate_hz) != ESP_OK
    ) {
        printf(
            "TARGET_STRESS_SCENARIO name=%s rate_hz=%u status=FAIL reason=init\n",
            name,
            (unsigned)rate_hz
        );
        heap_caps_free(phase->cache_storage);
        heap_caps_free(phase->recorder_batch);
        heap_caps_free(phase);
        return false;
    }

    log_heap_snapshot(phase, "start", true);
    const esp_timer_create_args_t timer_args = {
        .callback = stress_isr_callback,
        .arg = phase,
        .dispatch_method = ESP_TIMER_ISR,
        .name = "stress_isr",
    };
    if (esp_timer_create(&timer_args, &phase->isr_timer) == ESP_OK) {
        if (esp_timer_start_periodic(phase->isr_timer, 500U) == ESP_OK) {
            phase->isr_timer_started = true;
        } else {
            esp_timer_delete(phase->isr_timer);
            phase->isr_timer = NULL;
        }
    }

    phase->task_create_failed |= xTaskCreatePinnedToCore(
        producer_task, "stress_prod", 4096, phase, 8, &phase->producer_task, 1
    ) != pdPASS;
    phase->task_create_failed |= xTaskCreatePinnedToCore(
        recorder_task, "stress_rec", 6144, phase, 4, &phase->recorder_task, 0
    ) != pdPASS;
    phase->task_create_failed |= xTaskCreatePinnedToCore(
        ui_task, "stress_ui", 3072, phase, 3, &phase->ui_task, 0
    ) != pdPASS;
    phase->task_create_failed |= xTaskCreatePinnedToCore(
        sensor_task, "stress_sensor", 3072, phase, 2, &phase->sensor_task, 0
    ) != pdPASS;
    phase->task_create_failed |= xTaskCreatePinnedToCore(
        network_task, "stress_net", 3072, phase, 2, &phase->network_task, 0
    ) != pdPASS;
    phase->task_create_failed |= xTaskCreatePinnedToCore(
        scheduler_noise_task, "stress_sched", 3072, phase, 5, &phase->scheduler_task, 1
    ) != pdPASS;

    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    phase->running = false;
    while (
        phase->producer_task != NULL || phase->recorder_task != NULL ||
        phase->ui_task != NULL || phase->sensor_task != NULL ||
        phase->network_task != NULL || phase->scheduler_task != NULL
    ) {
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
    if (phase->isr_timer != NULL) {
        esp_timer_stop(phase->isr_timer);
        esp_timer_delete(phase->isr_timer);
    }

    log_heap_snapshot(phase, "end", false);
    dump_trace(phase);
    const bool saturated = phase_is_saturated(phase);
    const uint64_t expected_ticks = (uint64_t)duration_ms * 1000ULL / 500ULL;
    const uint64_t expected_min = expected_ticks > 10ULL
        ? expected_ticks - expected_ticks / 10ULL
        : expected_ticks;
    const bool isr_timing_ok =
        phase->isr_timer_started && phase->isr_ticks >= expected_min;
    const bool infrastructure_ok =
        schedule_ok && isr_timing_ok && !phase->task_create_failed;
    const bool verdict_pass = infrastructure_ok && saturated == expected_saturated;
    printf(
        "TARGET_STRESS_ISR period_us=500 ticks=%llu expected_min=%u status=%s\n",
        (unsigned long long)phase->isr_ticks,
        (unsigned)expected_min,
        isr_timing_ok ? "PASS" : "FAIL"
    );
    printf(
        "TARGET_STRESS_SCENARIO name=%s seed=%u rate_hz=%u duration_ms=%u "
        "produced=%llu recorder=%llu ring_dropped=%llu cache_overflow=%llu "
        "producer_deadlines=%llu recorder_errors=%llu isr_ticks=%llu "
        "trace_events=%u stack_prod=%u stack_rec=%u stack_ui=%u stack_sensor=%u "
        "stack_net=%u stack_sched=%u internal_start=%u internal_end=%u "
        "dma_start=%u dma_end=%u psram_start=%u psram_end=%u cache_psram=%u "
        "recorder_batch_psram=%u isr_timer=%u task_create_failed=%u "
        "observed=%s expected=%s verdict=%s\n",
        name,
        (unsigned)seed,
        (unsigned)rate_hz,
        (unsigned)duration_ms,
        (unsigned long long)phase->produced_frames,
        (unsigned long long)phase->recorder_frames,
        (unsigned long long)phase->ring_dropped_frames,
        (unsigned long long)phase->cache_overflow_frames,
        (unsigned long long)phase->producer_deadline_misses,
        (unsigned long long)phase->recorder_errors,
        (unsigned long long)phase->isr_ticks,
        (unsigned)phase->trace_write_index,
        (unsigned)phase->producer_stack_free_bytes,
        (unsigned)phase->recorder_stack_free_bytes,
        (unsigned)phase->ui_stack_free_bytes,
        (unsigned)phase->sensor_stack_free_bytes,
        (unsigned)phase->network_stack_free_bytes,
        (unsigned)phase->scheduler_stack_free_bytes,
        (unsigned)phase->internal_free_start,
        (unsigned)phase->internal_free_end,
        (unsigned)phase->dma_free_start,
        (unsigned)phase->dma_free_end,
        (unsigned)phase->psram_free_start,
        (unsigned)phase->psram_free_end,
        phase->cache_in_psram ? 1U : 0U,
        phase->recorder_batch_in_psram ? 1U : 0U,
        phase->isr_timer_started ? 1U : 0U,
        phase->task_create_failed ? 1U : 0U,
        saturated ? "SATURATED" : "PASS",
        expected_saturated ? "SATURATED" : "PASS",
        verdict_pass ? "PASS" : "FAIL"
    );
    heap_caps_free(phase->cache_storage);
    heap_caps_free(phase->recorder_batch);
    heap_caps_free(phase);
    return verdict_pass;
}


static uint32_t find_rate_boundary(void)
{
    uint32_t low = 0U;
    uint32_t high = STRESS_RATE_MAX_HZ;
    while (low + 1U < high) {
        const uint32_t middle = low + (high - low) / 2U;
        const bool pass = run_phase(
            "binary_search",
            middle,
            STRESS_BINARY_DURATION_MS,
            1U,
            4096U,
            false,
            STRESS_SEED
        );
        if (pass) {
            low = middle;
        } else {
            high = middle;
        }
    }
    printf(
        "TARGET_STRESS_BINARY_SEARCH seed=%u healthy_rate_hz=%u first_failed_rate_hz=%u\n",
        (unsigned)STRESS_SEED,
        (unsigned)low,
        (unsigned)high
    );
    return low;
}


void app_main(void)
{
    printf("TARGET_STRESS_BEGIN seed=%u core_count=%d\n", (unsigned)STRESS_SEED, portNUM_PROCESSORS);
    stress_physical_probe_run();
    if (seismic_buffer_init() != ESP_OK) {
        printf("TARGET_STRESS_SUMMARY: FAIL reason=buffer_init\n");
        return;
    }

    static const uint32_t rates[] = {100U, 250U, 500U, 1000U, 2000U, 4000U, 5000U};
    bool all_pass = true;
    for (size_t index = 0U; index < sizeof(rates) / sizeof(rates[0]); ++index) {
        const bool expected_saturated = rates[index] == STRESS_RATE_MAX_HZ;
        all_pass = run_phase(
            "rate_sweep",
            rates[index],
            STRESS_RATE_DURATION_MS,
            1U,
            4096U,
            expected_saturated,
            STRESS_SEED
        ) && all_pass;
    }

    const uint32_t healthy_rate = find_rate_boundary();
    (void)healthy_rate;
    all_pass = run_phase(
        "intentional_saturation",
        STRESS_RATE_MAX_HZ,
        STRESS_SATURATION_DURATION_MS,
        200U,
        512U,
        true,
        STRESS_SEED
    ) && all_pass;

    printf(
        "TARGET_STRESS_SUMMARY: %s seed=%u\n",
        all_pass ? "PASS" : "FAIL",
        (unsigned)STRESS_SEED
    );
}
