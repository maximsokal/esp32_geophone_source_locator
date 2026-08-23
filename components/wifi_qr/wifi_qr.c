#include "wifi_qr.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "app_config.h"
#include "display.h"

#include "esp_log.h"

#include "qrcode.h"


static const char *TAG =
    "WIFI_QR";


#define WIFI_QR_QUIET_ZONE_MODULES    4

#define WIFI_QR_MAX_VERSION           10


typedef struct
{
    int x;
    int y;
    int max_size;

    esp_err_t result;

} wifi_qr_render_context_t;


/*
 * Render one generated QR symbol into a complete
 * RGB565 bitmap and send it to the display.
 *
 * A four-module white quiet zone is included because
 * QR scanners require clear space around the symbol.
 */
static void wifi_qr_render_callback(
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


    const int qr_module_count =
        esp_qrcode_get_size(
            qrcode
        );


    if (qr_module_count <= 0) {

        context->result =
            ESP_FAIL;

        return;
    }


    const int total_module_count =
        qr_module_count +
        (
            WIFI_QR_QUIET_ZONE_MODULES *
            2
        );


    const int module_scale =
        context->max_size /
        total_module_count;


    if (module_scale < 1) {

        context->result =
            ESP_ERR_INVALID_SIZE;

        return;
    }


    /*
     * The full output bitmap always occupies max_size x max_size.
     *
     * Any unused pixels around the scaled QR code remain white.
     */
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


    /*
     * Start with a completely white bitmap.
     */
    for (
        size_t i = 0;
        i < pixel_count;
        ++i
    ) {
        bitmap[i] =
            DISPLAY_COLOR_WHITE;
    }


    const int rendered_size =
        total_module_count *
        module_scale;


    const int outer_offset =
        (
            context->max_size -
            rendered_size
        ) / 2;


    /*
     * Only black QR modules need to be written because
     * the bitmap and quiet zone are already white.
     */
    for (
        int module_y = 0;
        module_y < qr_module_count;
        ++module_y
    ) {
        for (
            int module_x = 0;
            module_x < qr_module_count;
            ++module_x
        ) {
            const bool black =
                esp_qrcode_get_module(
                    qrcode,
                    module_x,
                    module_y
                );


            if (!black) {
                continue;
            }


            const int pixel_x =
                outer_offset +
                (
                    module_x +
                    WIFI_QR_QUIET_ZONE_MODULES
                ) *
                module_scale;


            const int pixel_y =
                outer_offset +
                (
                    module_y +
                    WIFI_QR_QUIET_ZONE_MODULES
                ) *
                module_scale;


            for (
                int dy = 0;
                dy < module_scale;
                ++dy
            ) {
                for (
                    int dx = 0;
                    dx < module_scale;
                    ++dx
                ) {
                    const int x =
                        pixel_x + dx;

                    const int y =
                        pixel_y + dy;


                    bitmap[
                        (size_t)y *
                            context->max_size +
                        x
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


    if (context->result == ESP_OK) {

        ESP_LOGI(
            TAG,
            "Wi-Fi QR rendered: modules=%d scale=%d area=%dx%d",
            qr_module_count,
            module_scale,
            context->max_size,
            context->max_size
        );
    }


    free(bitmap);
}


esp_err_t wifi_qr_draw(
    int x,
    int y,
    int max_size
)
{
    if (
        x < 0 ||
        y < 0 ||
        max_size <= 0 ||
        x + max_size > DISPLAY_WIDTH ||
        y + max_size > DISPLAY_HEIGHT
    ) {
        return ESP_ERR_INVALID_ARG;
    }


    /*
     * Standard Wi-Fi QR payload.
     *
     * T:WPA is compatible with WPA/WPA2 Wi-Fi networks.
     */
    char payload[128];


    const int length =
        snprintf(
            payload,
            sizeof(payload),
            "WIFI:T:WPA;S:%s;P:%s;;",
            APP_WIFI_AP_SSID,
            APP_WIFI_AP_PASSWORD
        );


    if (
        length < 0 ||
        length >= (int)sizeof(payload)
    ) {
        return ESP_ERR_INVALID_SIZE;
    }


    /*
     * The upstream qrcode component logs the encoded text
     * at INFO level. Suppress that log so the Wi-Fi password
     * is not printed to the serial console.
     */
    esp_log_level_set(
        "QRCODE",
        ESP_LOG_WARN
    );


    wifi_qr_render_context_t context = {
        .x = x,
        .y = y,
        .max_size = max_size,
        .result = ESP_FAIL,
    };


    esp_qrcode_config_t config =
        ESP_QRCODE_CONFIG_DEFAULT();


    config.display_func_with_cb =
        wifi_qr_render_callback;

    config.user_data =
        &context;

    config.max_qrcode_version =
        WIFI_QR_MAX_VERSION;

    config.qrcode_ecc_level =
        ESP_QRCODE_ECC_LOW;


    ESP_LOGI(
        TAG,
        "Generating Wi-Fi connection QR"
    );


    esp_err_t err =
        esp_qrcode_generate(
            &config,
            payload
        );


    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "QR generation failed: %s",
            esp_err_to_name(err)
        );

        return err;
    }


    if (context.result != ESP_OK) {

        ESP_LOGE(
            TAG,
            "QR rendering failed: %s",
            esp_err_to_name(
                context.result
            )
        );

        return context.result;
    }


    return ESP_OK;
}