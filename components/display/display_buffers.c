#include "display_internal.h"

#include <stdlib.h>

#include "display.h"

#include "esp_heap_caps.h"
#include "esp_log.h"


static const char *TAG =
    "DISPLAY_BUFFERS";


static uint16_t *s_dma_buffer =
    NULL;


static size_t s_dma_capacity_pixels =
    0;


static uint16_t *s_text_buffer =
    NULL;


static size_t s_text_capacity_pixels =
    0;


bool display_buffers_is_ready(void)
{
    return
        s_dma_buffer != NULL &&
        s_text_buffer != NULL;
}


esp_err_t display_buffers_init(void)
{
    if (
        display_buffers_is_ready()
    ) {
        return ESP_OK;
    }

    s_dma_capacity_pixels =
        (size_t)DISPLAY_WIDTH *
        DISPLAY_TRANSFER_ROWS;

    s_dma_buffer =
        heap_caps_malloc(
            s_dma_capacity_pixels *
                sizeof(uint16_t),
            MALLOC_CAP_DMA |
                MALLOC_CAP_INTERNAL
        );

    if (s_dma_buffer == NULL) {
        ESP_LOGE(
            TAG,
            "Failed to allocate LCD DMA buffer"
        );

        display_buffers_deinit();

        return ESP_ERR_NO_MEM;
    }

    s_text_capacity_pixels =
        (size_t)DISPLAY_WIDTH *
        DISPLAY_TEXT_SCRATCH_ROWS;

    /*
     * Prefer PSRAM so future acquisition code keeps
     * more internal RAM available.
     */
    s_text_buffer =
        heap_caps_malloc(
            s_text_capacity_pixels *
                sizeof(uint16_t),
            MALLOC_CAP_SPIRAM |
                MALLOC_CAP_8BIT
        );

    if (s_text_buffer == NULL) {
        s_text_buffer =
            malloc(
                s_text_capacity_pixels *
                    sizeof(uint16_t)
            );
    }

    if (s_text_buffer == NULL) {
        ESP_LOGE(
            TAG,
            "Failed to allocate text scratch buffer"
        );

        display_buffers_deinit();

        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "Buffers ready: DMA=%u bytes text=%u bytes",
        (unsigned)(
            s_dma_capacity_pixels *
            sizeof(uint16_t)
        ),
        (unsigned)(
            s_text_capacity_pixels *
            sizeof(uint16_t)
        )
    );

    return ESP_OK;
}


void display_buffers_deinit(void)
{
    if (s_text_buffer != NULL) {
        free(
            s_text_buffer
        );

        s_text_buffer =
            NULL;
    }

    s_text_capacity_pixels =
        0;

    if (s_dma_buffer != NULL) {
        free(
            s_dma_buffer
        );

        s_dma_buffer =
            NULL;
    }

    s_dma_capacity_pixels =
        0;
}


uint16_t *display_buffers_dma(void)
{
    return
        s_dma_buffer;
}


size_t display_buffers_dma_capacity_pixels(void)
{
    return
        s_dma_capacity_pixels;
}


uint16_t *display_buffers_text(void)
{
    return
        s_text_buffer;
}


size_t display_buffers_text_capacity_pixels(void)
{
    return
        s_text_capacity_pixels;
}
