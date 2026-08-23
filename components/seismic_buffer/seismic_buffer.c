#include "seismic_buffer.h"

#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"


static SemaphoreHandle_t s_mutex = NULL;

static seismic_frame_t
    s_frames[SEISMIC_BUFFER_CAPACITY];

static size_t s_write_index = 0;
static size_t s_frame_count = 0;

static uint64_t s_total_frames = 0;

static uint32_t s_sample_rate_hz = 0;

static seismic_source_t s_source =
    SEISMIC_SOURCE_NONE;

static bool s_initialized = false;


esp_err_t seismic_buffer_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }


    s_mutex =
        xSemaphoreCreateMutex();


    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }


    memset(
        s_frames,
        0,
        sizeof(s_frames)
    );


    s_initialized = true;


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


    xSemaphoreTake(
        s_mutex,
        portMAX_DELAY
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


    memset(
        s_frames,
        0,
        sizeof(s_frames)
    );


    xSemaphoreGive(
        s_mutex
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


    xSemaphoreTake(
        s_mutex,
        portMAX_DELAY
    );


    if (
        s_source == SEISMIC_SOURCE_NONE ||
        s_sample_rate_hz == 0
    ) {
        xSemaphoreGive(
            s_mutex
        );

        return ESP_ERR_INVALID_STATE;
    }


    seismic_frame_t *frame =
        &s_frames[s_write_index];


    frame->timestamp_us =
        timestamp_us;


    for (
        size_t channel = 0;
        channel < SEISMIC_CHANNEL_COUNT;
        ++channel
    ) {
        frame->channel_uv[channel] =
            channel_uv[channel];
    }


    s_write_index++;


    if (
        s_write_index >=
        SEISMIC_BUFFER_CAPACITY
    ) {
        s_write_index = 0;
    }


    if (
        s_frame_count <
        SEISMIC_BUFFER_CAPACITY
    ) {
        s_frame_count++;
    }


    s_total_frames++;


    xSemaphoreGive(
        s_mutex
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


    xSemaphoreTake(
        s_mutex,
        portMAX_DELAY
    );


    size_t count =
        s_frame_count;


    if (count > max_frames) {
        count = max_frames;
    }


    size_t start_index =
        (
            s_write_index +
            SEISMIC_BUFFER_CAPACITY -
            count
        ) %
        SEISMIC_BUFFER_CAPACITY;


    for (
        size_t i = 0;
        i < count;
        ++i
    ) {
        size_t source_index =
            (
                start_index + i
            ) %
            SEISMIC_BUFFER_CAPACITY;


        frames[i] =
            s_frames[source_index];
    }


    *frame_count =
        count;


    info->source =
        s_source;

    info->sample_rate_hz =
        s_sample_rate_hz;

    info->buffered_frames =
        s_frame_count;

    info->total_frames =
        s_total_frames;


    xSemaphoreGive(
        s_mutex
    );


    return ESP_OK;
}


const char *seismic_buffer_source_name(
    seismic_source_t source
)
{
    switch (source) {

        case SEISMIC_SOURCE_TEST:
            return "TEST";

        case SEISMIC_SOURCE_AD7606:
            return "AD7606";

        case SEISMIC_SOURCE_NONE:
        default:
            return "NONE";
    }
}