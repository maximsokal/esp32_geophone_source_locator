#pragma once

#include "esp_err.h"
#include "driver/spi_master.h"


typedef esp_err_t (*app_spi_bus_quiesce_callback_t)(void *context);
typedef void (*app_spi_bus_resume_callback_t)(void *context);


esp_err_t app_spi_bus_init(void);
spi_host_device_t app_spi_bus_get_host(void);

/*
 * Runtime arbitration for the shared SPI2 bus.
 *
 * The TFT uses interrupt/queued transfers through esp_lcd while SDSPI uses
 * polling transfers internally. ESP-IDF requires polling transfers to start
 * only after outstanding interrupt transfers on the same host have finished.
 *
 * The UI registers a quiesce/resume pair that stops new LVGL flushes and
 * drains queued LCD transactions. Storage operations then enter this guard
 * before touching FAT/SDSPI.
 */
esp_err_t app_spi_bus_register_display_guard(
    app_spi_bus_quiesce_callback_t quiesce,
    app_spi_bus_resume_callback_t resume,
    void *context
);

void app_spi_bus_unregister_display_guard(void);

esp_err_t app_spi_bus_sd_operation_begin(void);
void app_spi_bus_sd_operation_end(void);
