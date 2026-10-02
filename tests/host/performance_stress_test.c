#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "seismic_buffer.h"
#include "seismic_recorder_cache.h"


/*
 * Software-only virtual-board stress model. This file must never acquire a
 * serial port, flash an image, or call a physical peripheral driver.
 */
#define STRESS_SPI3_CLOCK_HZ             1000000U
#define STRESS_SPI2_CLOCK_HZ            20000000U
#define STRESS_AD7606_FRAME_BYTES             16U
#define STRESS_TFT_BUFFER_BYTES              5120U
#define STRESS_SD_CHUNK_FRAMES               256U
#define STRESS_RECORDER_SERVICE_US        10000ULL
#define STRESS_TFT_OVERHEAD_US                80U
#define STRESS_SD_OVERHEAD_US                180U
#define STRESS_SPI3_OVERHEAD_US               12U
#define STRESS_SPI2_QUEUE_CAPACITY             8U
#define STRESS_MAX_BUS_TRANSACTIONS           16U
#define STRESS_MAX_LATENCY_SAMPLES           4096U
#define STRESS_MAX_RECORDER_BATCH             512U
#define VIRTUAL_WIFI_EVENT_INTERVAL_US     50000ULL
#define VIRTUAL_I2C_TRANSACTION_INTERVAL_US 25000ULL


typedef enum
{
    STRESS_TRANSACTION_SD = 0,
    STRESS_TRANSACTION_TFT

} stress_transaction_kind_t;


typedef struct
{
    uint64_t completion_us;
    stress_transaction_kind_t kind;
    uint64_t first_session_frame;
    size_t frame_count;

} stress_transaction_t;


typedef struct
{
    uint64_t busy_until_us;
    uint64_t busy_us;
    size_t transaction_count;
    size_t queue_high_water;
    uint64_t queue_overflows;
    stress_transaction_t transactions[STRESS_MAX_BUS_TRANSACTIONS];

} virtual_spi2_t;


typedef struct
{
    uint64_t conversions;
    uint64_t busy_pulses;
    uint64_t varied_payloads;
    uint64_t timeouts;

} virtual_ad7606_t;


typedef struct
{
    uint64_t events;
    uint64_t dropped_events;

} virtual_wifi_t;


typedef struct
{
    uint64_t transactions;
    uint64_t nacks;

} virtual_i2c_t;


typedef struct
{
    virtual_spi2_t spi2;
    virtual_ad7606_t ad7606;
    virtual_wifi_t wifi;
    virtual_i2c_t i2c;

} virtual_board_t;


typedef struct
{
    const char *name;
    const char *workload_id;
    uint32_t seed;
    uint32_t sample_rate_hz;
    uint32_t duration_ms;
    uint32_t drain_ms;
    size_t cache_capacity_frames;
    uint32_t sd_base_us;
    uint32_t sd_jitter_us;
    uint32_t stall_start_ms;
    uint32_t stall_duration_ms;
    uint32_t tft_interval_us;
    uint32_t dsp_us;
    uint32_t background_us;
    bool expected_saturated;
    bool enable_wifi;
    bool enable_i2c;

} stress_scenario_t;


typedef struct
{
    uint64_t produced_frames;
    uint64_t recorder_frames;
    uint64_t persisted_frames;
    uint64_t ring_dropped_frames;
    uint64_t cache_overflow_frames;
    uint64_t cache_overflow_events;
    uint64_t producer_errors;
    uint64_t cache_errors;
    uint64_t spi3_deadline_misses;
    uint64_t cpu_deadline_misses;
    uint64_t tft_queue_overflows;
    uint64_t storage_submissions;
    uint64_t storage_completions;
    uint64_t max_backlog_frames;
    size_t max_cache_frames;
    size_t max_ring_frames;
    uint64_t spi3_busy_us;
    uint64_t spi2_busy_us;
    uint64_t virtual_ad7606_conversions;
    uint64_t virtual_ad7606_busy_pulses;
    uint64_t virtual_ad7606_varied_payloads;
    uint64_t virtual_ad7606_timeouts;
    uint64_t virtual_wifi_events;
    uint64_t virtual_wifi_dropped_events;
    uint64_t virtual_i2c_transactions;
    uint64_t virtual_i2c_nacks;
    uint64_t virtual_sd_transactions;
    uint64_t virtual_tft_frames;
    uint64_t sd_latencies_us[STRESS_MAX_LATENCY_SAMPLES];
    size_t sd_latency_count;
    bool frame_duplication_detected;
    bool frame_reordering_detected;
    bool channel_swap_detected;
    uint64_t last_ingested_frame;

} stress_metrics_t;


typedef struct stress_context stress_context_t;


struct stress_context
{
    const stress_scenario_t *scenario;
    uint64_t random_state;
    uint64_t next_service_us;
    uint64_t next_tft_us;
    uint64_t next_background_us;
    uint64_t recorder_cursor;
    uint64_t spi3_busy_until_us;
    uint64_t next_wifi_event_us;
    uint64_t next_i2c_transaction_us;
    bool storage_inflight;
    seismic_recorder_cache_t cache;
    seismic_recorder_cache_slot_t *cache_storage;
    virtual_board_t board;
    stress_metrics_t metrics;
};


static uint64_t wire_time_us(size_t bytes, uint32_t clock_hz)
{
    const uint64_t bits = (uint64_t)bytes * 8ULL;
    return (bits * 1000000ULL + (uint64_t)clock_hz - 1ULL) /
           (uint64_t)clock_hz;
}


static uint32_t next_random(stress_context_t *context)
{
    uint64_t value = context->random_state;
    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    context->random_state = value;
    return (uint32_t)value;
}


static void record_sd_latency(
    stress_metrics_t *metrics,
    uint64_t latency_us
)
{
    if (metrics->sd_latency_count < STRESS_MAX_LATENCY_SAMPLES) {
        metrics->sd_latencies_us[metrics->sd_latency_count++] = latency_us;
    }
}


static int compare_u64(const void *left, const void *right)
{
    const uint64_t left_value = *(const uint64_t *)left;
    const uint64_t right_value = *(const uint64_t *)right;
    return left_value < right_value ? -1 : left_value > right_value ? 1 : 0;
}


static uint64_t percentile_u64(
    const uint64_t *values,
    size_t count,
    unsigned percentile
)
{
    if (count == 0U) {
        return 0U;
    }

    uint64_t sorted[STRESS_MAX_LATENCY_SAMPLES];
    assert(count <= STRESS_MAX_LATENCY_SAMPLES);
    memcpy(sorted, values, count * sizeof(sorted[0]));
    qsort(sorted, count, sizeof(sorted[0]), compare_u64);

    const size_t rank = ((size_t)percentile * count + 99U) / 100U;
    const size_t index = rank == 0U ? 0U : rank - 1U;
    return sorted[index < count ? index : count - 1U];
}


static void update_cache_metrics(stress_context_t *context)
{
    seismic_recorder_cache_info_t info = {0};
    assert(seismic_recorder_cache_get_info(&context->cache, &info) == ESP_OK);

    if (info.dirty_frames > context->metrics.max_backlog_frames) {
        context->metrics.max_backlog_frames = info.dirty_frames;
    }
    if (info.resident_frames > context->metrics.max_cache_frames) {
        context->metrics.max_cache_frames = info.resident_frames;
    }
}


static void virtual_spi2_advance(
    stress_context_t *context,
    uint64_t now_us
)
{
    virtual_spi2_t *bus = &context->board.spi2;

    while (
        bus->transaction_count > 0U &&
        bus->transactions[0].completion_us <= now_us
    ) {
        const stress_transaction_t completed = bus->transactions[0];

        if (completed.kind == STRESS_TRANSACTION_SD) {
            uint64_t last_source_frame = 0U;
            const esp_err_t err = seismic_recorder_cache_mark_persisted(
                &context->cache,
                completed.first_session_frame,
                completed.frame_count,
                &last_source_frame
            );
            if (err != ESP_OK) {
                context->metrics.cache_errors++;
            } else {
                context->metrics.storage_completions++;
                context->metrics.persisted_frames +=
                    (uint64_t)completed.frame_count;
            }
            context->storage_inflight = false;
        }

        memmove(
            &bus->transactions[0],
            &bus->transactions[1],
            (bus->transaction_count - 1U) * sizeof(bus->transactions[0])
        );
        bus->transaction_count--;
    }
}


static bool virtual_spi2_submit(
    stress_context_t *context,
    uint64_t now_us,
    stress_transaction_kind_t kind,
    size_t bytes,
    uint64_t extra_duration_us,
    uint64_t first_session_frame,
    size_t frame_count
)
{
    virtual_spi2_t *bus = &context->board.spi2;
    virtual_spi2_advance(context, now_us);

    if (
        bus->transaction_count >= STRESS_SPI2_QUEUE_CAPACITY ||
        bus->transaction_count >= STRESS_MAX_BUS_TRANSACTIONS
    ) {
        bus->queue_overflows++;
        return false;
    }

    const uint64_t start_us = bus->busy_until_us > now_us
        ? bus->busy_until_us
        : now_us;
    const uint64_t transfer_us = wire_time_us(bytes, STRESS_SPI2_CLOCK_HZ);
    const uint64_t duration_us = transfer_us + extra_duration_us;
    const uint64_t completion_us = start_us + duration_us;

    bus->transactions[bus->transaction_count++] = (stress_transaction_t){
        .completion_us = completion_us,
        .kind = kind,
        .first_session_frame = first_session_frame,
        .frame_count = frame_count,
    };
    bus->busy_until_us = completion_us;
    bus->busy_us += duration_us;
    if (bus->transaction_count > bus->queue_high_water) {
        bus->queue_high_water = bus->transaction_count;
    }

    if (kind == STRESS_TRANSACTION_SD) {
        context->storage_inflight = true;
        context->metrics.storage_submissions++;
        context->metrics.virtual_sd_transactions++;
        record_sd_latency(&context->metrics, completion_us - now_us);
    } else {
        context->metrics.virtual_tft_frames++;
    }

    return true;
}


static void virtual_ad7606_service(
    stress_context_t *context,
    uint64_t now_us,
    uint64_t period_us,
    uint64_t transfer_completion_us
)
{
    virtual_ad7606_t *ad7606 = &context->board.ad7606;
    ad7606->conversions++;
    ad7606->busy_pulses++;
    if (transfer_completion_us > now_us + period_us) {
        ad7606->timeouts++;
    } else {
        ad7606->varied_payloads++;
    }
}


static void virtual_wifi_service(
    stress_context_t *context,
    uint64_t now_us
)
{
    if (!context->scenario->enable_wifi || now_us < context->next_wifi_event_us) {
        return;
    }

    context->board.wifi.events++;
    context->next_wifi_event_us = now_us + VIRTUAL_WIFI_EVENT_INTERVAL_US;
}


static void virtual_i2c_service(
    stress_context_t *context,
    uint64_t now_us
)
{
    if (!context->scenario->enable_i2c || now_us < context->next_i2c_transaction_us) {
        return;
    }

    context->board.i2c.transactions++;
    context->next_i2c_transaction_us =
        now_us + VIRTUAL_I2C_TRANSACTION_INTERVAL_US;
}


static void virtual_board_copy_metrics(
    stress_context_t *context
)
{
    context->metrics.virtual_ad7606_conversions =
        context->board.ad7606.conversions;
    context->metrics.virtual_ad7606_busy_pulses =
        context->board.ad7606.busy_pulses;
    context->metrics.virtual_ad7606_varied_payloads =
        context->board.ad7606.varied_payloads;
    context->metrics.virtual_ad7606_timeouts =
        context->board.ad7606.timeouts;
    context->metrics.virtual_wifi_events = context->board.wifi.events;
    context->metrics.virtual_wifi_dropped_events =
        context->board.wifi.dropped_events;
    context->metrics.virtual_i2c_transactions = context->board.i2c.transactions;
    context->metrics.virtual_i2c_nacks = context->board.i2c.nacks;
}


static bool virtual_board_protocols_healthy(
    const stress_metrics_t *metrics,
    const stress_scenario_t *scenario
)
{
    return metrics->virtual_ad7606_conversions > 0U &&
           metrics->virtual_ad7606_busy_pulses ==
               metrics->virtual_ad7606_conversions &&
           metrics->virtual_ad7606_varied_payloads ==
               metrics->virtual_ad7606_conversions &&
           metrics->virtual_ad7606_timeouts == 0U &&
           metrics->virtual_wifi_dropped_events == 0U &&
           metrics->virtual_i2c_nacks == 0U &&
           (!scenario->enable_wifi || metrics->virtual_wifi_events > 0U) &&
           (scenario->enable_wifi || metrics->virtual_wifi_events == 0U) &&
           (!scenario->enable_i2c || metrics->virtual_i2c_transactions > 0U) &&
           (scenario->enable_i2c || metrics->virtual_i2c_transactions == 0U) &&
           metrics->virtual_sd_transactions > 0U &&
           (scenario->tft_interval_us == 0U
                ? metrics->virtual_tft_frames == 0U
                : metrics->virtual_tft_frames > 0U);
}


static bool sd_is_stalled(
    const stress_scenario_t *scenario,
    uint64_t now_us
)
{
    const uint64_t start_us = (uint64_t)scenario->stall_start_ms * 1000ULL;
    const uint64_t end_us = start_us +
        (uint64_t)scenario->stall_duration_ms * 1000ULL;
    return scenario->stall_duration_ms > 0U &&
           now_us >= start_us &&
           now_us < end_us;
}


static void recorder_ingest(stress_context_t *context)
{
    seismic_frame_t batch[STRESS_MAX_RECORDER_BATCH];
    size_t frame_count = 0U;
    uint64_t dropped_frames = 0U;
    seismic_buffer_info_t info = {0};
    uint64_t candidate_cursor = context->recorder_cursor;

    const esp_err_t copy_err = seismic_buffer_copy_since(
        &candidate_cursor,
        batch,
        STRESS_MAX_RECORDER_BATCH,
        &frame_count,
        &dropped_frames,
        &info
    );
    if (copy_err != ESP_OK) {
        context->metrics.cache_errors++;
        return;
    }

    if (info.buffered_frames > context->metrics.max_ring_frames) {
        context->metrics.max_ring_frames = info.buffered_frames;
    }
    if (frame_count == 0U) {
        context->metrics.ring_dropped_frames += dropped_frames;
        context->recorder_cursor = candidate_cursor;
        return;
    }

    const uint64_t first_source_frame =
        candidate_cursor - (uint64_t)frame_count + 1ULL;
    const esp_err_t append_err = seismic_recorder_cache_append(
        &context->cache,
        first_source_frame,
        batch,
        frame_count
    );
    if (append_err != ESP_OK) {
        if (append_err == ESP_ERR_NO_MEM) {
            context->metrics.cache_overflow_events++;
            context->metrics.cache_overflow_frames += frame_count;
        } else {
            context->metrics.cache_errors++;
        }
        return;
    }

    context->metrics.ring_dropped_frames += dropped_frames;
    for (size_t index = 0U; index < frame_count; ++index) {
        const float sequence = batch[index].channel_uv[0];
        const uint64_t frame_number = (uint64_t)sequence;
        if (
            sequence <= 0.0f ||
            batch[index].channel_uv[1] != -sequence ||
            batch[index].channel_uv[2] != sequence + 0.25f ||
            batch[index].channel_uv[3] != sequence - 0.25f
        ) {
            context->metrics.channel_swap_detected = true;
        }
        if (frame_number == context->metrics.last_ingested_frame) {
            context->metrics.frame_duplication_detected = true;
        } else if (frame_number < context->metrics.last_ingested_frame) {
            context->metrics.frame_reordering_detected = true;
        }
        context->metrics.last_ingested_frame = frame_number;
    }

    context->recorder_cursor = candidate_cursor;
    context->metrics.recorder_frames += (uint64_t)frame_count;
    update_cache_metrics(context);
}


static void recorder_flush(stress_context_t *context, uint64_t now_us)
{
    if (
        context->storage_inflight ||
        sd_is_stalled(context->scenario, now_us)
    ) {
        return;
    }

    seismic_frame_t batch[STRESS_SD_CHUNK_FRAMES];
    size_t frame_count = 0U;
    uint64_t first_session_frame = 0U;
    assert(seismic_recorder_cache_copy_dirty(
        &context->cache,
        batch,
        STRESS_SD_CHUNK_FRAMES,
        &frame_count,
        &first_session_frame
    ) == ESP_OK);
    if (frame_count == 0U) {
        return;
    }

    uint64_t jitter_us = 0U;
    if (context->scenario->sd_jitter_us > 0U) {
        jitter_us = (uint64_t)(next_random(context) %
            context->scenario->sd_jitter_us);
    }

    const uint64_t extra_us =
        (uint64_t)context->scenario->sd_base_us +
        jitter_us +
        STRESS_SD_OVERHEAD_US;
    if (!virtual_spi2_submit(
        context,
        now_us,
        STRESS_TRANSACTION_SD,
        frame_count * sizeof(seismic_frame_t),
        extra_us,
        first_session_frame,
        frame_count
    )) {
        context->metrics.cache_errors++;
    }
}


static void recorder_service(stress_context_t *context, uint64_t now_us)
{
    if (now_us < context->next_service_us) {
        return;
    }

    context->next_service_us += STRESS_RECORDER_SERVICE_US;
    recorder_ingest(context);
    recorder_flush(context, now_us);
    update_cache_metrics(context);
}


static void tft_service(stress_context_t *context, uint64_t now_us)
{
    if (
        context->scenario->tft_interval_us == 0U ||
        now_us < context->next_tft_us
    ) {
        return;
    }

    context->next_tft_us = now_us + context->scenario->tft_interval_us;
    if (!virtual_spi2_submit(
        context,
        now_us,
        STRESS_TRANSACTION_TFT,
        STRESS_TFT_BUFFER_BYTES,
        STRESS_TFT_OVERHEAD_US,
        0U,
        0U
    )) {
        context->metrics.tft_queue_overflows++;
    }
}


static void publish_frame(
    stress_context_t *context,
    uint64_t frame_number,
    uint64_t now_us
)
{
    const uint64_t period_us = 1000000ULL /
        (uint64_t)context->scenario->sample_rate_hz;
    const uint64_t spi3_start = context->spi3_busy_until_us > now_us
        ? context->spi3_busy_until_us
        : now_us;
    const uint64_t spi3_completion = spi3_start +
        wire_time_us(STRESS_AD7606_FRAME_BYTES, STRESS_SPI3_CLOCK_HZ) +
        STRESS_SPI3_OVERHEAD_US;
    if (spi3_completion > now_us + period_us) {
        context->metrics.spi3_deadline_misses++;
    }
    context->spi3_busy_until_us = spi3_completion;
    context->metrics.spi3_busy_us += spi3_completion - spi3_start;
    virtual_ad7606_service(
        context,
        now_us,
        period_us,
        spi3_completion
    );

    uint64_t cpu_work_us = context->scenario->dsp_us;
    if (now_us >= context->next_background_us) {
        cpu_work_us += context->scenario->background_us;
        context->next_background_us += 100000ULL;
    }
    if (cpu_work_us > period_us) {
        context->metrics.cpu_deadline_misses++;
    }

    float channels[SEISMIC_CHANNEL_COUNT] = {0};
    channels[0] = (float)frame_number;
    channels[1] = -(float)frame_number;
    channels[2] = (float)frame_number + 0.25f;
    channels[3] = (float)frame_number - 0.25f;
    if (seismic_buffer_push((int64_t)now_us, channels) != ESP_OK) {
        context->metrics.producer_errors++;
    } else {
        context->metrics.produced_frames++;
    }
}


static stress_metrics_t run_scenario(
    const stress_scenario_t *scenario,
    size_t *bus_queue_high_water,
    uint64_t *bus_queue_overflows,
    uint64_t *sd_p99_us
)
{
    stress_context_t context = {
        .scenario = scenario,
        .random_state = scenario->seed,
        .next_service_us = STRESS_RECORDER_SERVICE_US,
        .next_tft_us = 0U,
        .next_background_us = 100000ULL,
        .next_wifi_event_us = 0U,
        .next_i2c_transaction_us = 0U,
        .board = {0},
    };
    context.cache_storage = calloc(
        scenario->cache_capacity_frames,
        sizeof(*context.cache_storage)
    );
    assert(context.cache_storage != NULL);
    assert(seismic_recorder_cache_init(
        &context.cache,
        context.cache_storage,
        scenario->cache_capacity_frames
    ) == ESP_OK);

    assert(seismic_buffer_init() == ESP_OK);
    assert(seismic_buffer_configure(
        SEISMIC_SOURCE_AD7606,
        scenario->sample_rate_hz
    ) == ESP_OK);

    const uint64_t period_us = 1000000ULL /
        (uint64_t)scenario->sample_rate_hz;
    const uint64_t duration_us = (uint64_t)scenario->duration_ms * 1000ULL;
    const uint64_t frame_count =
        ((uint64_t)scenario->duration_ms * scenario->sample_rate_hz) / 1000ULL;

    for (uint64_t frame = 1U; frame <= frame_count; ++frame) {
        const uint64_t now_us = frame * period_us;
        virtual_spi2_advance(&context, now_us);
        virtual_wifi_service(&context, now_us);
        virtual_i2c_service(&context, now_us);
        publish_frame(&context, frame, now_us);
        tft_service(&context, now_us);
        recorder_service(&context, now_us);
    }

    const uint64_t drain_end_us = duration_us +
        (uint64_t)scenario->drain_ms * 1000ULL;
    for (
        uint64_t now_us = duration_us;
        now_us <= drain_end_us;
        now_us += period_us
    ) {
        virtual_spi2_advance(&context, now_us);
        virtual_wifi_service(&context, now_us);
        virtual_i2c_service(&context, now_us);
        recorder_service(&context, now_us);
    }
    virtual_spi2_advance(&context, drain_end_us);
    update_cache_metrics(&context);
    virtual_board_copy_metrics(&context);

    *bus_queue_high_water = context.board.spi2.queue_high_water;
    *bus_queue_overflows = context.board.spi2.queue_overflows;
    *sd_p99_us = percentile_u64(
        context.metrics.sd_latencies_us,
        context.metrics.sd_latency_count,
        99U
    );
    context.metrics.spi2_busy_us = context.board.spi2.busy_us;
    free(context.cache_storage);
    return context.metrics;
}


static bool has_fault(
    const stress_metrics_t *metrics,
    uint64_t spi2_queue_overflows
)
{
    return metrics->ring_dropped_frames > 0U ||
           metrics->cache_overflow_frames > 0U ||
           metrics->producer_errors > 0U ||
           metrics->cache_errors > 0U ||
           metrics->spi3_deadline_misses > 0U ||
           metrics->cpu_deadline_misses > 0U ||
           spi2_queue_overflows > 0U;
}


static void print_scenario_result(
    const stress_scenario_t *scenario,
    const stress_metrics_t *metrics,
    size_t bus_queue_high_water,
    uint64_t bus_queue_overflows,
    uint64_t sd_p99_us,
    bool verdict_pass
)
{
    const bool saturated = has_fault(metrics, bus_queue_overflows);
    printf(
        "{\"type\":\"stress_scenario\",\"name\":\"%s\","
        "\"seed\":%" PRIu32 ",\"sample_rate_hz\":%" PRIu32 ","
        "\"produced_frames\":%" PRIu64 ",\"recorder_frames\":%" PRIu64 ","
        "\"ring_dropped_frames\":%" PRIu64 ","
        "\"cache_overflow_frames\":%" PRIu64 ","
        "\"cache_high_water\":%zu,\"backlog_high_water\":%" PRIu64 ","
        "\"spi3_deadline_misses\":%" PRIu64 ","
        "\"cpu_deadline_misses\":%" PRIu64 ","
        "\"spi2_queue_high_water\":%zu,\"spi2_queue_overflows\":%" PRIu64 ","
        "\"storage_submissions\":%" PRIu64 ","
        "\"storage_completions\":%" PRIu64 ","
        "\"sd_p99_latency_us\":%" PRIu64 ","
        "\"virtual_ad7606_conversions\":%" PRIu64 ","
        "\"virtual_ad7606_busy_pulses\":%" PRIu64 ","
        "\"virtual_ad7606_varied_payloads\":%" PRIu64 ","
        "\"virtual_ad7606_timeouts\":%" PRIu64 ","
        "\"virtual_wifi_events\":%" PRIu64 ","
        "\"virtual_i2c_transactions\":%" PRIu64 ","
        "\"virtual_sd_transactions\":%" PRIu64 ","
        "\"virtual_tft_frames\":%" PRIu64 ","
        "\"virtual_board\":\"%s\","
        "\"observed\":\"%s\",\"expected\":\"%s\","
        "\"verdict\":\"%s\"}\n",
        scenario->name,
        scenario->seed,
        scenario->sample_rate_hz,
        metrics->produced_frames,
        metrics->recorder_frames,
        metrics->ring_dropped_frames,
        metrics->cache_overflow_frames,
        metrics->max_cache_frames,
        metrics->max_backlog_frames,
        metrics->spi3_deadline_misses,
        metrics->cpu_deadline_misses,
        bus_queue_high_water,
        bus_queue_overflows,
        metrics->storage_submissions,
        metrics->storage_completions,
        sd_p99_us,
        metrics->virtual_ad7606_conversions,
        metrics->virtual_ad7606_busy_pulses,
        metrics->virtual_ad7606_varied_payloads,
        metrics->virtual_ad7606_timeouts,
        metrics->virtual_wifi_events,
        metrics->virtual_i2c_transactions,
        metrics->virtual_sd_transactions,
        metrics->virtual_tft_frames,
        virtual_board_protocols_healthy(metrics, scenario) ? "PASS" : "FAIL",
        saturated ? "SATURATED" : "PASS",
        scenario->expected_saturated ? "SATURATED" : "PASS",
        verdict_pass ? "PASS" : "FAIL"
    );
}


static void print_perf_record(
    const stress_scenario_t *scenario,
    const stress_metrics_t *metrics,
    size_t bus_queue_high_water,
    bool saturated
)
{
    assert(metrics->persisted_frames + metrics->ring_dropped_frames <=
           metrics->produced_frames);
    const uint64_t resident_frames = metrics->produced_frames -
        metrics->persisted_frames - metrics->ring_dropped_frames;
    const uint64_t duration_ms = (uint64_t)scenario->duration_ms +
        (uint64_t)scenario->drain_ms;
    const double writes_per_second = duration_ms == 0U
        ? 0.0
        : (double)metrics->storage_completions * 1000.0 /
            (double)duration_ms;
    const double bytes_per_write = metrics->storage_completions == 0U
        ? 0.0
        : (double)(metrics->persisted_frames * sizeof(seismic_frame_t)) /
            (double)metrics->storage_completions;

    printf(
        "PERF_RECORD_JSON: {\"schema_version\":1,"
        "\"firmware_sha256\":\"not_applicable\","
        "\"build_profile\":\"not_applicable\","
        "\"optimization_mode\":\"not_applicable\","
        "\"test_surface\":\"HOST_VIRTUAL\","
        "\"sample_rate_hz\":%" PRIu32 ",\"channel_count\":%d,"
        "\"sample_format\":\"float32_uv\","
        "\"channel_sentinel_order\":[\"frame\",\"negative_frame\","
        "\"frame_plus_quarter\",\"frame_minus_quarter\"],"
        "\"sd_card_identity\":\"not_applicable\","
        "\"sd_card_frequency_hz\":\"not_applicable\","
        "\"workload_id\":\"%s\",\"seed\":%" PRIu32 ","
        "\"duration_ms\":%" PRIu64 ",\"classification\":\"%s\","
        "\"produced_frames\":%" PRIu64 ",\"ingested_frames\":%" PRIu64 ","
        "\"persisted_frames\":%" PRIu64 ","
        "\"explicitly_dropped_frames\":%" PRIu64 ","
        "\"explicitly_rejected_frames\":0,\"resident_frames\":%" PRIu64 ","
        "\"acquisition_service_jitter_us\":\"not_applicable\","
        "\"recorder_write_latency_us\":{\"p50\":%" PRIu64 ","
        "\"p95\":%" PRIu64 ",\"p99\":%" PRIu64 ",\"max\":%" PRIu64 "},"
        "\"bytes_per_write\":%.3f,\"writes_per_second\":%.3f,"
        "\"ring_high_water_frames\":%zu,\"cache_high_water_frames\":%zu,"
        "\"queue_high_water\":%zu,"
        "\"cpu_runtime_us\":\"not_applicable\","
        "\"task_runtime_us\":\"not_applicable\","
        "\"core_runtime_us\":\"not_applicable\","
        "\"internal_heap_free_bytes\":\"not_applicable\","
        "\"internal_heap_largest_block_bytes\":\"not_applicable\","
        "\"psram_free_bytes\":\"not_applicable\","
        "\"psram_largest_block_bytes\":\"not_applicable\","
        "\"stack_hwm_words\":\"not_applicable\","
        "\"web_stop_latency_ms\":\"not_applicable\","
        "\"web_status_latency_ms\":\"not_applicable\","
        "\"live_latency_ms\":\"not_applicable\","
        "\"reset_markers\":{\"unexpected_reset\":\"not_applicable\","
        "\"watchdog\":\"not_applicable\",\"brownout\":\"not_applicable\"},"
        "\"project_hot_path_allocation_violations\":\"not_applicable\","
        "\"active_allocation_count\":\"not_applicable\","
        "\"diagnostic_reports_suppressed\":\"not_applicable\","
        "\"diagnostic_reports_coalesced\":\"not_applicable\","
        "\"v2_quantizer_calls\":\"not_applicable\","
        "\"raw_path_selection\":\"not_applicable\","
        "\"seismic_buffer_atomic_operations\":\"not_applicable\","
        "\"seismic_buffer_critical_section_entries\":\"not_applicable\","
        "\"seismic_buffer_retries\":\"not_applicable\","
        "\"ad7606_status_critical_section_entries\":\"not_applicable\","
        "\"ad7606_status_critical_section_total_us\":\"not_applicable\","
        "\"ad7606_status_critical_section_max_us\":\"not_applicable\","
        "\"frame_duplication_detected\":%s,"
        "\"frame_reordering_detected\":%s,\"channel_swap_detected\":%s,"
        "\"corrupted_finalized_file\":\"not_applicable\","
        "\"corrupted_finalized_session\":\"not_applicable\","
        "\"dirty_cache_overwrite_detected\":false,"
        "\"diagnostic_integer_overflow_detected\":\"not_applicable\","
        "\"deadlock_detected\":false,\"workload_supported\":true,"
        "\"unsupported_workload_disposition\":\"not_applicable\"}\n",
        scenario->sample_rate_hz,
        SEISMIC_CHANNEL_COUNT,
        scenario->workload_id,
        scenario->seed,
        duration_ms,
        saturated ? "SATURATED" : "PASS",
        metrics->produced_frames,
        metrics->recorder_frames,
        metrics->persisted_frames,
        metrics->ring_dropped_frames,
        resident_frames,
        percentile_u64(metrics->sd_latencies_us, metrics->sd_latency_count, 50U),
        percentile_u64(metrics->sd_latencies_us, metrics->sd_latency_count, 95U),
        percentile_u64(metrics->sd_latencies_us, metrics->sd_latency_count, 99U),
        percentile_u64(metrics->sd_latencies_us, metrics->sd_latency_count, 100U),
        bytes_per_write,
        writes_per_second,
        metrics->max_ring_frames,
        metrics->max_cache_frames,
        bus_queue_high_water,
        metrics->frame_duplication_detected ? "true" : "false",
        metrics->frame_reordering_detected ? "true" : "false",
        metrics->channel_swap_detected ? "true" : "false"
    );
}


static void test_virtual_board_stress_scenarios(void)
{
    static const stress_scenario_t scenarios[] = {
        {
            .name = "baseline_500hz",
            .workload_id = "healthy",
            .seed = 0x84AE1732U,
            .sample_rate_hz = 500U,
            .duration_ms = 3000U,
            .drain_ms = 500U,
            .cache_capacity_frames = 4096U,
            .sd_base_us = 500U,
            .sd_jitter_us = 500U,
            .tft_interval_us = 100000U,
            .dsp_us = 200U,
            .background_us = 600U,
            .expected_saturated = false,
            .enable_wifi = true,
            .enable_i2c = true,
        },
        {
            .name = "sd_stall_200ms",
            .workload_id = "sd_stall_200ms",
            .seed = 0x84AE1732U,
            .sample_rate_hz = 500U,
            .duration_ms = 3000U,
            .drain_ms = 800U,
            .cache_capacity_frames = 4096U,
            .sd_base_us = 500U,
            .sd_jitter_us = 500U,
            .stall_start_ms = 1000U,
            .stall_duration_ms = 200U,
            .tft_interval_us = 50000U,
            .dsp_us = 250U,
            .background_us = 700U,
            .expected_saturated = false,
            .enable_wifi = true,
            .enable_i2c = true,
        },
        {
            .name = "burst_shared_spi2",
            .workload_id = "shared_spi_burst",
            .seed = 0x84AE1732U,
            .sample_rate_hz = 500U,
            .duration_ms = 2500U,
            .drain_ms = 600U,
            .cache_capacity_frames = 4096U,
            .sd_base_us = 2000U,
            .sd_jitter_us = 4000U,
            .tft_interval_us = 4000U,
            .dsp_us = 1200U,
            .background_us = 900U,
            .expected_saturated = true,
            .enable_wifi = true,
            .enable_i2c = true,
        },
        {
            .name = "steady_5000hz",
            .workload_id = "steady_5000hz",
            .seed = 0x84AE1732U,
            .sample_rate_hz = 5000U,
            .duration_ms = 250U,
            .drain_ms = 300U,
            .cache_capacity_frames = 4096U,
            .sd_base_us = 1000U,
            .sd_jitter_us = 500U,
            .tft_interval_us = 16000U,
            .dsp_us = 100U,
            .background_us = 50U,
            .expected_saturated = false,
            .enable_wifi = true,
            .enable_i2c = true,
        },
        {
            .name = "saturation_5000hz_sd_slow",
            .workload_id = "intentional_saturation",
            .seed = 0x84AE1732U,
            .sample_rate_hz = 5000U,
            .duration_ms = 250U,
            .drain_ms = 100U,
            .cache_capacity_frames = 512U,
            .sd_base_us = 90000U,
            .sd_jitter_us = 5000U,
            .tft_interval_us = 4000U,
            .dsp_us = 100U,
            .background_us = 300U,
            .expected_saturated = true,
            .enable_wifi = true,
            .enable_i2c = true,
        },
        {
            .name = "sd_jitter_500hz",
            .workload_id = "sd_jitter",
            .seed = 0x84AE1732U,
            .sample_rate_hz = 500U,
            .duration_ms = 2000U,
            .drain_ms = 500U,
            .cache_capacity_frames = 4096U,
            .sd_base_us = 500U,
            .sd_jitter_us = 8000U,
            .tft_interval_us = 100000U,
            .dsp_us = 300U,
            .background_us = 400U,
            .expected_saturated = false,
            .enable_wifi = true,
            .enable_i2c = true,
        },
        /* A 256-frame recorder batch at 500 Hz spans 512 ms. */
        {
            .name = "stall_below_batch_interval",
            .workload_id = "stall_below_batch_interval",
            .seed = 0x84AE1732U,
            .sample_rate_hz = 500U,
            .duration_ms = 2200U,
            .drain_ms = 1000U,
            .cache_capacity_frames = 4096U,
            .sd_base_us = 500U,
            .sd_jitter_us = 500U,
            .stall_start_ms = 700U,
            .stall_duration_ms = 250U,
            .tft_interval_us = 100000U,
            .dsp_us = 250U,
            .background_us = 300U,
            .expected_saturated = false,
            .enable_wifi = true,
            .enable_i2c = true,
        },
        {
            .name = "stall_around_batch_interval",
            .workload_id = "stall_around_batch_interval",
            .seed = 0x84AE1732U,
            .sample_rate_hz = 500U,
            .duration_ms = 2200U,
            .drain_ms = 1000U,
            .cache_capacity_frames = 4096U,
            .sd_base_us = 500U,
            .sd_jitter_us = 500U,
            .stall_start_ms = 700U,
            .stall_duration_ms = 512U,
            .tft_interval_us = 100000U,
            .dsp_us = 250U,
            .background_us = 300U,
            .expected_saturated = false,
            .enable_wifi = true,
            .enable_i2c = true,
        },
        {
            .name = "stall_above_batch_interval",
            .workload_id = "stall_above_batch_interval",
            .seed = 0x84AE1732U,
            .sample_rate_hz = 500U,
            .duration_ms = 2600U,
            .drain_ms = 1200U,
            .cache_capacity_frames = 4096U,
            .sd_base_us = 500U,
            .sd_jitter_us = 500U,
            .stall_start_ms = 700U,
            .stall_duration_ms = 750U,
            .tft_interval_us = 100000U,
            .dsp_us = 250U,
            .background_us = 300U,
            .expected_saturated = false,
            .enable_wifi = true,
            .enable_i2c = true,
        },
        {
            .name = "tft_contention",
            .workload_id = "tft_contention",
            .seed = 0x84AE1732U,
            .sample_rate_hz = 500U,
            .duration_ms = 2000U,
            .drain_ms = 500U,
            .cache_capacity_frames = 4096U,
            .sd_base_us = 1000U,
            .sd_jitter_us = 1000U,
            .tft_interval_us = 8000U,
            .dsp_us = 400U,
            .background_us = 500U,
            .expected_saturated = false,
            .enable_wifi = true,
            .enable_i2c = true,
        },
        {
            .name = "wifi_activity",
            .workload_id = "wifi_activity",
            .seed = 0x84AE1732U,
            .sample_rate_hz = 500U,
            .duration_ms = 1000U,
            .drain_ms = 300U,
            .cache_capacity_frames = 2048U,
            .sd_base_us = 500U,
            .sd_jitter_us = 500U,
            .tft_interval_us = 0U,
            .dsp_us = 500U,
            .background_us = 900U,
            .expected_saturated = false,
            .enable_wifi = true,
            .enable_i2c = false,
        },
        {
            .name = "i2c_activity",
            .workload_id = "i2c_activity",
            .seed = 0x84AE1732U,
            .sample_rate_hz = 500U,
            .duration_ms = 1000U,
            .drain_ms = 300U,
            .cache_capacity_frames = 2048U,
            .sd_base_us = 500U,
            .sd_jitter_us = 500U,
            .tft_interval_us = 0U,
            .dsp_us = 500U,
            .background_us = 700U,
            .expected_saturated = false,
            .enable_wifi = false,
            .enable_i2c = true,
        },
        {
            .name = "worst_case_admitted",
            .workload_id = "worst_case_admitted",
            .seed = 0x84AE1732U,
            .sample_rate_hz = 500U,
            .duration_ms = 3000U,
            .drain_ms = 800U,
            .cache_capacity_frames = 4096U,
            .sd_base_us = 2000U,
            .sd_jitter_us = 4000U,
            .tft_interval_us = 16000U,
            .dsp_us = 1000U,
            .background_us = 800U,
            .expected_saturated = false,
            .enable_wifi = true,
            .enable_i2c = true,
        },
    };

    size_t failed = 0U;
    for (size_t index = 0U; index < sizeof(scenarios) / sizeof(scenarios[0]); ++index) {
        size_t queue_high_water = 0U;
        uint64_t queue_overflows = 0U;
        uint64_t sd_p99_us = 0U;
        const stress_metrics_t metrics = run_scenario(
            &scenarios[index],
            &queue_high_water,
            &queue_overflows,
            &sd_p99_us
        );
        const bool saturated = has_fault(&metrics, queue_overflows);
        const bool verdict_pass =
            virtual_board_protocols_healthy(&metrics, &scenarios[index]) &&
            saturated == scenarios[index].expected_saturated;
        print_scenario_result(
            &scenarios[index],
            &metrics,
            queue_high_water,
            queue_overflows,
            sd_p99_us,
            verdict_pass
        );
        print_perf_record(
            &scenarios[index],
            &metrics,
            queue_high_water,
            saturated
        );
        if (!verdict_pass) {
            fprintf(
                stderr,
                "STRESS_SCENARIO_FAILED: name=%s seed=0x%08" PRIx32 "\n",
                scenarios[index].name,
                scenarios[index].seed
            );
            failed++;
        }
    }

    printf(
        "{\"type\":\"virtual_board_stress_summary\",\"scenarios\":%zu,"
        "\"failed\":%zu,\"verdict\":\"%s\"}\n",
        sizeof(scenarios) / sizeof(scenarios[0]),
        failed,
        failed == 0U ? "PASS" : "FAIL"
    );
    printf(
        "VIRTUAL_BOARD_STRESS_RESULT: %s physical_io=NONE com_ports=NONE\n",
        failed == 0U ? "PASS" : "FAIL"
    );
    assert(failed == 0U);
}


int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_virtual_board_stress_scenarios();
    puts("virtual board stress tests passed");
    return 0;
}
