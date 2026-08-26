#include "wifi_qr_render.h"

#include <stdint.h>
#include <stdlib.h>

#include "display.h"

#include "esp_log.h"


static const char *TAG =
    "WIFI_QR";


#define WIFI_QR_QUIET_ZONE_MODULES    4


void wifi_qr_render_callback(
    esp_qrcode_handle_t qrcode,
    void *user_data
)
{
    wifi_qr_render_context_t *context =
        (wifi_qr_render_context_t *)
            user_data;

    if (context == NULL) {
        return;
    }

    const int module_count =
        esp_qrcode_get_size(
            qrcode
        );

    if (module_count <= 0) {
        context->result =
            ESP_FAIL;

        return;
    }

    const int total_modules =
        module_count +
        WIFI_QR_QUIET_ZONE_MODULES * 2;

    const int scale =
        context->max_size /
        total_modules;

    if (scale < 1) {
        context->result =
            ESP_ERR_INVALID_SIZE;

        return;
    }

    const size_t pixel_count =
        (size_t)context->max_size *
        context->max_size;

    uint16_t *bitmap =
        malloc(
            pixel_count *
            sizeof(uint16_t)
        );

    if (bitmap == NULL) {
        context->result =
            ESP_ERR_NO_MEM;

        return;
    }

    for (
        size_t i = 0;
        i < pixel_count;
        ++i
    ) {
        bitmap[i] =
            DISPLAY_COLOR_WHITE;
    }

    const int rendered_size =
        total_modules *
        scale;

    const int offset =
        (
            context->max_size -
            rendered_size
        ) / 2;

    for (
        int module_y = 0;
        module_y < module_count;
        ++module_y
    ) {
        for (
            int module_x = 0;
            module_x < module_count;
            ++module_x
        ) {
            if (
                !esp_qrcode_get_module(
                    qrcode,
                    module_x,
                    module_y
                )
            ) {
                continue;
            }

            const int pixel_x =
                offset +
                (
                    module_x +
                    WIFI_QR_QUIET_ZONE_MODULES
                ) * scale;

            const int pixel_y =
                offset +
                (
                    module_y +
                    WIFI_QR_QUIET_ZONE_MODULES
                ) * scale;

            for (
                int dy = 0;
                dy < scale;
                ++dy
            ) {
                for (
                    int dx = 0;
                    dx < scale;
                    ++dx
                ) {
                    bitmap[
                        (size_t)(
                            pixel_y + dy
                        ) *
                        context->max_size +
                        pixel_x +
                        dx
                    ] =
                        DISPLAY_COLOR_BLACK;
                }
            }
        }
    }

    context->result =
        display_draw_rgb565_bitmap(
            context->x,
            context->y,
            context->max_size,
            context->max_size,
            bitmap
        );

    if (
        context->result ==
        ESP_OK
    ) {
        ESP_LOGI(
            TAG,
            "Setup QR rendered: modules=%d scale=%d area=%dx%d",
            module_count,
            scale,
            context->max_size,
            context->max_size
        );
    }

    free(
        bitmap
    );
}
