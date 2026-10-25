#include <assert.h>
#include <stdio.h>

#include "ui_internal.h"
#include "ui_perf_policy.h"
#include "display.h"
#include "spi_bus.h"
#include "esp_lvgl_port.h"
#include "freertos/task.h"

static ui_context_t s_context;
static int s_io_token;
static bool s_missing_io;
static esp_err_t s_drain_result;
static int64_t s_now_us = 1000;
static unsigned s_barriers;
static unsigned s_delayed_ticks;

ui_context_t *ui_context(void) { return &s_context; }
int64_t esp_timer_get_time(void) { return s_now_us; }
bool display_is_ready(void) { return true; }
esp_lcd_panel_io_handle_t display_io_handle(void) { return s_missing_io ? NULL : &s_io_token; }
esp_lcd_panel_handle_t display_panel_handle(void) { return &s_io_token; }
bool lvgl_port_lock(uint32_t timeout_ms) { (void)timeout_ms; return true; }
void lvgl_port_unlock(void) {}
esp_err_t lvgl_port_init(const lvgl_port_cfg_t *config) { assert(config != NULL); return ESP_OK; }
esp_err_t lvgl_port_deinit(void) { return ESP_OK; }

lv_display_t *lvgl_port_add_disp(const lvgl_port_display_cfg_t *config)
{
    assert(config->flags.buff_dma && !config->flags.buff_spiram);
    assert(!config->double_buffer);
    return lv_test_display_create((int32_t)config->hres, (int32_t)config->vres);
}

esp_err_t lvgl_port_remove_disp(lv_display_t *display)
{
    lv_display_delete(display);
    return ESP_OK;
}

esp_err_t app_spi_bus_register_display_guard(app_spi_bus_quiesce_callback_t quiesce,
    app_spi_bus_resume_callback_t resume, void *context)
{
    assert(quiesce != NULL && resume != NULL);
    (void)context;
    return ESP_OK;
}
void app_spi_bus_unregister_display_guard(void) {}

esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io, int command,
    const void *data, size_t size)
{
    assert(io == &s_io_token && command == -1 && data == NULL && size == 0U);
    ++s_barriers;
    s_now_us += 250;
    return s_drain_result;
}

void vTaskDelay(TickType_t ticks) { s_delayed_ticks += ticks; s_now_us += 10000; }

static void wait_for_flush(void)
{
    lv_display_send_event(s_context.display, LV_EVENT_FLUSH_WAIT_START, NULL);
    lv_display_send_event(s_context.display, LV_EVENT_FLUSH_WAIT_FINISH, NULL);
}

int main(void)
{
    lv_init();
    assert(ui_lvgl_init() == ESP_OK);
    wait_for_flush();
    assert(s_barriers == 1U && s_delayed_ticks == 0U);
    s_drain_result = ESP_ERR_TIMEOUT;
    wait_for_flush();
    assert(s_barriers == 2U && s_delayed_ticks == 1U);
    s_missing_io = true;
    wait_for_flush();
    assert(s_barriers == 2U && s_delayed_ticks == 2U);
    s_missing_io = false;
    s_drain_result = ESP_OK;
    wait_for_flush();
    assert(s_barriers == 3U && s_delayed_ticks == 2U);
    ui_lvgl_deinit();
    assert(s_barriers == 4U && !s_context.port_ready && s_context.display == NULL);
    puts("ui_lvgl_wait: PASS");
    return 0;
}
