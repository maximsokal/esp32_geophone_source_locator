#include "seismic_buffer.h"

#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"


#define SEISMIC_SLOT_READ_RETRIES      4
#define SEISMIC_SNAPSHOT_RETRIES       4


typedef struct
{
    uint32_t sequence;
    uint64_t frame_number;
    seismic_frame_t frame;

} seismic_slot_t;


typedef struct
{
    size_t frame_count;
    uint64_t total_frames;
    uint32_t sample_rate_hz;
    seismic_source_t source;

} seismic_metadata_t;


static portMUX_TYPE s_metadata_lock =
    portMUX_INITIALIZER_UNLOCKED;

static seismic_slot_t
    s_slots[SEISMIC_BUFFER_CAPACITY];

static size_t s_write_index = 0;
static size_t s_frame_count = 0;
static uint64_t s_total_frames = 0;
static uint32_t s_sample_rate_hz = 0;

static seismic_source_t s_source =
    SEISMIC_SOURCE_NONE;

static bool s_initialized = false;


static void seismic_buffer_snapshot_metadata(
    seismic_metadata_t *metadata
)
{
    portENTER_CRITICAL(
        &s_metadata_lock
    );

    metadata->frame_count =
        s_frame_count;

    metadata->total_frames =
        s_total_frames;

    metadata->sample_rate_hz =
        s_sample_rate_hz;

    metadata->source =
        s_source;

    portEXIT_CRITICAL(
        &s_metadata_lock
    );
}


static void seismic_buffer_publish_info(
    const seismic_metadata_t *metadata,
    seismic_buffer_info_t *info
)
{
    info->source =
        metadata->source;

    info->sample_rate_hz =
        metadata->sample_rate_hz;

    info->buffered_frames =
        metadata->frame_count;

    info->total_frames =
        metadata->total_frames;
}


static void seismic_buffer_write_slot(
    seismic_slot_t *slot,
    uint64_t frame_number,
    int64_t timestamp_us,
    const float channel_uv[SEISMIC_CHANNEL_COUNT]
)
{
    uint32_t sequence =
        __atomic_load_n(
            &slot->sequence,
            __ATOMIC_SEQ_CST
        );

    if ((sequence & 1U) != 0U) {
        sequence++;
    }

    /*
     * Odd sequence means the producer is mutating this slot.
     * Readers never wait for the producer; they simply retry.
     */
    __atomic_store_n(
        &slot->sequence,
        sequence + 1U,
        __ATOMIC_SEQ_CST
    );

    slot->frame_number =
        frame_number;

    slot->frame.timestamp_us =
        timestamp_us;

    for (
        size_t channel = 0;
        channel < SEISMIC_CHANNEL_COUNT;
        ++channel
    ) {
        slot->frame.channel_uv[channel] =
            channel_uv[channel];
    }

    /*
     * Publishing an even sequence releases the completed frame.
     */
    __atomic_store_n(
        &slot->sequence,
        sequence + 2U,
        __ATOMIC_SEQ_CST
    );
}


static bool seismic_buffer_read_slot(
    const seismic_slot_t *slot,
    uint64_t expected_frame_number,
    seismic_frame_t *frame
)
{
    for (
        unsigned attempt = 0;
        attempt < SEISMIC_SLOT_READ_RETRIES;
        ++attempt
    ) {
        const uint32_t before =
            __atomic_load_n(
                &slot->sequence,
                __ATOMIC_SEQ_CST
            );

        if ((before & 1U) != 0U) {
            continue;
        }

        const uint64_t frame_number =
            slot->frame_number;

        const seismic_frame_t candidate =
            slot->frame;

        const uint32_t after =
            __atomic_load_n(
                &slot->sequence,
                __ATOMIC_SEQ_CST
            );

        if (
            before == after &&
            (after & 1U) == 0U &&
            frame_number == expected_frame_number
        ) {
            *frame =
                candidate;

            return true;
        }
    }

    return false;
}


esp_err_t seismic_buffer_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    memset(
        s_slots,
        0,
        sizeof(s_slots)
    );

    s_write_index =
        0;

    s_frame_count =
        0;

    s_total_frames =
        0;

    s_sample_rate_hz =
        0;

    s_source =
        SEISMIC_SOURCE_NONE;

    s_initialized =
        true;

    return ESP_OK;
}


esp_err_t seismic_buffer_configure(
    seismic_source_t source,
    uint32_t sample_rate_hz
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (
        source != SEISMIC_SOURCE_NONE &&
        sample_rate_hz == 0
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Reset only published metadata. Old slot contents are harmless:
     * frame_count becomes zero and every new session republishes slots
     * with frame numbers starting at one. Avoiding a bulk memset here
     * also avoids racing a web reader during shutdown/restart.
     */
    portENTER_CRITICAL(
        &s_metadata_lock
    );

    s_source =
        source;

    s_sample_rate_hz =
        sample_rate_hz;

    s_write_index =
        0;

    s_frame_count =
        0;

    s_total_frames =
        0;

    portEXIT_CRITICAL(
        &s_metadata_lock
    );

    return ESP_OK;
}


esp_err_t seismic_buffer_push(
    int64_t timestamp_us,
    const float channel_uv[SEISMIC_CHANNEL_COUNT]
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (channel_uv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t write_index;
    uint64_t frame_number;

    /*
     * This critical section only snapshots producer metadata.
     * Web/UI readers never hold it while copying waveform data.
     */
    portENTER_CRITICAL(
        &s_metadata_lock
    );

    if (
        s_source == SEISMIC_SOURCE_NONE ||
        s_sample_rate_hz == 0
    ) {
        portEXIT_CRITICAL(
            &s_metadata_lock
        );

        return ESP_ERR_INVALID_STATE;
    }

    write_index =
        s_write_index;

    frame_number =
        s_total_frames + 1U;

    portEXIT_CRITICAL(
        &s_metadata_lock
    );

    seismic_buffer_write_slot(
        &s_slots[write_index],
        frame_number,
        timestamp_us,
        channel_uv
    );

    /*
     * Publish ring metadata only after the slot is complete.
     */
    portENTER_CRITICAL(
        &s_metadata_lock
    );

    s_write_index =
        write_index + 1U;

    if (
        s_write_index >=
        SEISMIC_BUFFER_CAPACITY
    ) {
        s_write_index =
            0;
    }

    if (
        s_frame_count <
        SEISMIC_BUFFER_CAPACITY
    ) {
        s_frame_count++;
    }

    s_total_frames =
        frame_number;

    portEXIT_CRITICAL(
        &s_metadata_lock
    );

    return ESP_OK;
}


esp_err_t seismic_buffer_copy_latest(
    seismic_frame_t *frames,
    size_t max_frames,
    size_t *frame_count,
    seismic_buffer_info_t *info
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (
        frames == NULL ||
        frame_count == NULL ||
        info == NULL ||
        max_frames == 0
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    for (
        unsigned snapshot_attempt = 0;
        snapshot_attempt < SEISMIC_SNAPSHOT_RETRIES;
        ++snapshot_attempt
    ) {
        seismic_metadata_t metadata;

        seismic_buffer_snapshot_metadata(
            &metadata
        );

        size_t count =
            metadata.frame_count;

        if (count > max_frames) {
            count =
                max_frames;
        }

        seismic_buffer_publish_info(
            &metadata,
            info
        );

        if (count == 0) {
            *frame_count =
                0;

            return ESP_OK;
        }

        const uint64_t first_frame_number =
            metadata.total_frames -
            (uint64_t)count +
            1U;

        bool consistent =
            true;

        for (
            size_t i = 0;
            i < count;
            ++i
        ) {
            const uint64_t expected_frame_number =
                first_frame_number +
                (uint64_t)i;

            const size_t source_index =
                (size_t)(
                    (expected_frame_number - 1U) %
                    SEISMIC_BUFFER_CAPACITY
                );

            if (
                !seismic_buffer_read_slot(
                    &s_slots[source_index],
                    expected_frame_number,
                    &frames[i]
                )
            ) {
                consistent =
                    false;

                break;
            }
        }

        if (consistent) {
            *frame_count =
                count;

            return ESP_OK;
        }
    }

    /*
     * A producer wrap raced every retry. This is transient and,
     * importantly, never stalls the acquisition task.
     */
    return ESP_ERR_TIMEOUT;
}


esp_err_t seismic_buffer_copy_since(
    uint64_t *cursor,
    seismic_frame_t *frames,
    size_t max_frames,
    size_t *frame_count,
    uint64_t *dropped_frames,
    seismic_buffer_info_t *info
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (
        cursor == NULL ||
        frames == NULL ||
        frame_count == NULL ||
        dropped_frames == NULL ||
        info == NULL ||
        max_frames == 0
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    *frame_count = 0;
    *dropped_frames = 0;

    for (
        unsigned snapshot_attempt = 0;
        snapshot_attempt < SEISMIC_SNAPSHOT_RETRIES;
        ++snapshot_attempt
    ) {
        seismic_metadata_t metadata;
        seismic_buffer_snapshot_metadata(&metadata);
        seismic_buffer_publish_info(&metadata, info);

        if (*cursor > metadata.total_frames) {
            return ESP_ERR_INVALID_STATE;
        }

        if (metadata.frame_count == 0 || *cursor == metadata.total_frames) {
            return ESP_OK;
        }

        const uint64_t oldest_frame_number =
            metadata.total_frames -
            (uint64_t)metadata.frame_count +
            1U;

        uint64_t first_frame_number =
            *cursor + 1U;

        uint64_t dropped = 0;
        if (first_frame_number < oldest_frame_number) {
            dropped =
                oldest_frame_number -
                first_frame_number;
            first_frame_number =
                oldest_frame_number;
        }

        uint64_t available =
            metadata.total_frames -
            first_frame_number +
            1U;

        size_t count =
            available > (uint64_t)max_frames
                ? max_frames
                : (size_t)available;

        bool consistent = true;
        for (size_t i = 0; i < count; ++i) {
            const uint64_t expected_frame_number =
                first_frame_number +
                (uint64_t)i;

            const size_t source_index =
                (size_t)(
                    (expected_frame_number - 1U) %
                    SEISMIC_BUFFER_CAPACITY
                );

            if (!seismic_buffer_read_slot(
                    &s_slots[source_index],
                    expected_frame_number,
                    &frames[i]
                )) {
                consistent = false;
                break;
            }
        }

        if (!consistent) {
            continue;
        }

        *dropped_frames = dropped;
        *frame_count = count;
        *cursor =
            first_frame_number +
            (uint64_t)count -
            1U;
        return ESP_OK;
    }

    return ESP_ERR_TIMEOUT;
}


const char *seismic_buffer_source_name(
    seismic_source_t source
)
{
    switch (source) {
        case SEISMIC_SOURCE_TEST:
            return "TEST";

        case SEISMIC_SOURCE_ESP_ADC_BENCH:
            return "ESP_ADC_BENCH";

        case SEISMIC_SOURCE_AD7606:
            return "AD7606";

        case SEISMIC_SOURCE_NONE:
        default:
            return "NONE";
    }
}
