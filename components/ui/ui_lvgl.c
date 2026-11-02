#include "ui_internal.h"

#include <stdint.h>
#include <string.h>

#include "app_task_config.h"
#include "display.h"
#include "spi_bus.h"
#include "ui_perf_policy.h"

#ifdef ESP_PLATFORM
#include "esp_clk_tree.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#endif
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


static const char *TAG =
    "UI_LVGL";


/*
 * Keep one TFT draw buffer in DMA-capable internal SRAM, but do not make it
 * smaller than the rendering path can use efficiently. A 4-row buffer made a
 * full 320x240 invalidation 60 separate SPI flushes; 8 rows halves that
 * transaction count while costing only 320 * 8 * 2 = 5120 bytes. The buffer
 * remains single and internal so SDSPI/Wi-Fi DMA headroom is still explicit.
 */
#define UI_LVGL_SPI_CLOCK_HZ 40000000U

_Static_assert(
    ((uint64_t)DISPLAY_WIDTH * UI_PERF_FLUSH_CHUNK_ROWS * sizeof(uint16_t) * 8U * 1000000U +
     UI_LVGL_SPI_CLOCK_HZ - 1U) / UI_LVGL_SPI_CLOCK_HZ <=
        UI_PERF_FLUSH_CHUNK_BUDGET_US,
    "One LVGL TFT chunk must fit within the 5 ms wire-time budget"
);


static uint64_t s_render_started_us;
static uint64_t s_render_flush_submit_us;
static uint64_t s_render_flush_wait_us;
static uint64_t s_flush_submit_started_us;
static uint64_t s_flush_wait_started_us;
static uint64_t s_flush_submit_us;
static uint64_t s_flush_wait_us;
static bool s_flush_wait_yielded;
static uint64_t s_draw_started_us;     /* LVGL starts drawing the next chunk when the previous transfer is done */
static uint32_t s_frame_chunks;

static esp_err_t ui_lvgl_drain_panel_io_locked(void);

/*
 * The cost of a flush chunk (~20 ms on the bench) is the time LVGL's code and data need, not the SPI transfer. This
 * build runs the CPU at 160 MHz with a 16 KB instruction cache and DIO flash, so code and read-only data that miss
 * the cache are slow, and PSRAM is slower again. The probe reads every cache line of a region larger than the data
 * cache and reports the CPU cycles per read, so the log shows the speed of each memory that LVGL touches. The
 * reads of internal RAM include ~4 cycles of loop; flash and PSRAM numbers are what a cache miss costs.
 */
#ifdef ESP_PLATFORM
#define UI_MEM_PROBE_STRIDE_BYTES 64U
#define UI_MEM_PROBE_FLASH_PSRAM_BYTES 131072U
#define UI_MEM_PROBE_INTERNAL_BYTES 16384U
#define UI_MEM_PROBE_PASSES 3U

extern const uint8_t _rodata_start[];
extern const uint8_t _rodata_end[];
static volatile uint32_t s_mem_probe_sink;

static uint32_t ui_lvgl_probe_cycles_per_read(const volatile uint8_t *base, size_t bytes)
{
    uint32_t best = UINT32_MAX;
    for (unsigned pass = 0U; pass < UI_MEM_PROBE_PASSES; ++pass) {
        uint32_t sum = 0U;
        const uint32_t started = esp_cpu_get_cycle_count();
        for (size_t offset = 0U; offset < bytes; offset += UI_MEM_PROBE_STRIDE_BYTES) sum += base[offset];
        const uint32_t cycles = esp_cpu_get_cycle_count() - started;
        s_mem_probe_sink = sum;
        if (cycles < best) best = cycles;        /* a pass that was preempted reads too slow; the best one is right */
    }
    return best / (uint32_t)(bytes / UI_MEM_PROBE_STRIDE_BYTES);
}

static void ui_lvgl_log_memory_speed(void)
{
    uint32_t cpu_hz = 0U;
    (void)esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &cpu_hz);

    const size_t rodata_bytes = (size_t)(_rodata_end - _rodata_start);
    const size_t flash_bytes = rodata_bytes < UI_MEM_PROBE_FLASH_PSRAM_BYTES ? rodata_bytes : UI_MEM_PROBE_FLASH_PSRAM_BYTES;
    uint8_t *psram = heap_caps_malloc(UI_MEM_PROBE_FLASH_PSRAM_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *internal = heap_caps_malloc(UI_MEM_PROBE_INTERNAL_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (psram != NULL) memset(psram, 0x5A, UI_MEM_PROBE_FLASH_PSRAM_BYTES);
    if (internal != NULL) memset(internal, 0x5A, UI_MEM_PROBE_INTERNAL_BYTES);

    ESP_LOGI(
        TAG,
        "UI_MEM_PROBE cpu_mhz=%u icache_bytes=%u flash_mode=%s flash_mhz=%s psram_mhz=%u flash_cycles_per_read=%d psram_cycles_per_read=%d internal_cycles_per_read=%d region_bytes=%u",
        (unsigned)(cpu_hz / 1000000U),
        (unsigned)CONFIG_ESP32S3_INSTRUCTION_CACHE_SIZE,
        CONFIG_ESPTOOLPY_FLASHMODE,
        CONFIG_ESPTOOLPY_FLASHFREQ,
        (unsigned)CONFIG_SPIRAM_SPEED,
        flash_bytes >= UI_MEM_PROBE_STRIDE_BYTES
            ? (int)ui_lvgl_probe_cycles_per_read(_rodata_start, flash_bytes) : -1,
        psram != NULL ? (int)ui_lvgl_probe_cycles_per_read(psram, UI_MEM_PROBE_FLASH_PSRAM_BYTES) : -1,
        internal != NULL ? (int)ui_lvgl_probe_cycles_per_read(internal, UI_MEM_PROBE_INTERNAL_BYTES) : -1,
        (unsigned)flash_bytes
    );
    heap_caps_free(psram);
    heap_caps_free(internal);
}
#else
/* The native render and wait fixtures have no flash cache or PSRAM to probe. */
static void ui_lvgl_log_memory_speed(void) {}
#endif

static void ui_lvgl_perf_event(lv_event_t *event)
{
    if (event == NULL) return;
    const uint64_t now_us = (uint64_t)esp_timer_get_time();

    switch (lv_event_get_code(event)) {
        case LV_EVENT_RENDER_START:
            s_render_started_us = now_us;
            s_render_flush_submit_us = s_flush_submit_us;
            s_render_flush_wait_us = s_flush_wait_us;
            s_draw_started_us = now_us;
            s_frame_chunks = 0U;
            break;

        case LV_EVENT_RENDER_READY:
            if (s_render_started_us != 0U && now_us >= s_render_started_us) {
                const uint64_t elapsed_us = now_us - s_render_started_us;
                const uint64_t flush_us =
                    (s_flush_submit_us - s_render_flush_submit_us) +
                    (s_flush_wait_us - s_render_flush_wait_us);
                ui_perf_policy_record_render(elapsed_us > flush_us ? elapsed_us - flush_us : 0U);
            }
            ui_perf_policy_record_frame_chunks(s_frame_chunks);
            s_render_started_us = 0U;
            break;

        case LV_EVENT_FLUSH_START: {
            /* The draw of this chunk ran from the end of the previous transfer (or the frame start) to here. */
            const lv_area_t *area = lv_event_get_param(event);
            const uint32_t pixels = area != NULL
                ? (uint32_t)(area->x2 - area->x1 + 1) * (uint32_t)(area->y2 - area->y1 + 1)
                : 0U;
            if (s_draw_started_us != 0U && now_us >= s_draw_started_us) {
                ui_perf_policy_record_chunk_draw(now_us - s_draw_started_us, pixels);
            }
            ++s_frame_chunks;
            s_flush_submit_started_us = now_us;
            break;
        }

        case LV_EVENT_FLUSH_FINISH:
            if (s_flush_submit_started_us != 0U && now_us >= s_flush_submit_started_us) {
                const uint64_t elapsed_us = now_us - s_flush_submit_started_us;
                ui_perf_policy_record_flush_submit(elapsed_us);
                s_flush_submit_us += elapsed_us;
            }
            s_flush_submit_started_us = 0U;
            break;

        case LV_EVENT_FLUSH_WAIT_START:
            s_flush_wait_started_us = now_us;
            /* Block on the existing SPI completion barrier instead of LVGL's spin. */
            s_flush_wait_yielded = true;
            if (ui_lvgl_drain_panel_io_locked() != ESP_OK) {
                vTaskDelay(1U);
            }
            break;

        case LV_EVENT_FLUSH_WAIT_FINISH:
            if (s_flush_wait_started_us != 0U && now_us >= s_flush_wait_started_us) {
                const uint64_t elapsed_us = now_us - s_flush_wait_started_us;
                ui_perf_policy_record_flush_wait(elapsed_us, s_flush_wait_yielded);
                s_flush_wait_us += elapsed_us;
            }
            s_flush_wait_started_us = 0U;
            s_flush_wait_yielded = false;
            s_draw_started_us = now_us;
            break;

        default:
            break;
    }
}


static esp_err_t ui_lvgl_drain_panel_io_locked(void)
{
    esp_lcd_panel_io_handle_t io =
        display_io_handle();

    if (io == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * esp_lcd_panel_io_tx_param() is a synchronous polling transaction. The
     * SPI LCD implementation first waits for every queued tx_color operation
     * to complete. lcd_cmd=-1, param=NULL, size=0 emits no command, so this is
     * a pure queue-drain barrier.
     *
     * Do not install another on_color_trans_done callback here. esp_lvgl_port
     * owns that callback after lvgl_port_add_disp(); overwriting it breaks the
     * port's flush-completion contract and produced the runtime warning:
     * "Callback on_color_trans_done was already set and now it was overwritten".
     */
    return esp_lcd_panel_io_tx_param(
        io,
        -1,
        NULL,
        0
    );
}


static esp_err_t ui_lvgl_quiesce_shared_spi(void *context)
{
    (void)context;

    /*
     * Stop the LVGL task from submitting another asynchronous color flush.
     * A zero timeout in esp_lvgl_port means wait indefinitely. Once locked,
     * drain the LCD IO queue before SDSPI starts using the shared SPI2 bus.
     */
    if (!lvgl_port_lock(0)) {
        return ESP_ERR_TIMEOUT;
    }

    const esp_err_t err =
        ui_lvgl_drain_panel_io_locked();

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to drain LCD SPI queue before SD access: %s",
            esp_err_to_name(err)
        );

        lvgl_port_unlock();
        return err;
    }

    return ESP_OK;
}


static void ui_lvgl_resume_shared_spi(void *context)
{
    (void)context;
    lvgl_port_unlock();
}


esp_err_t ui_lvgl_init(void)
{
    ui_context_t *context =
        ui_context();

    if (context->port_ready) {
        return ESP_OK;
    }

    if (!display_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    lvgl_port_cfg_t port_config =
        ESP_LVGL_PORT_INIT_CONFIG();

    port_config.task_priority =
        APP_TASK_PRIORITY_LVGL;

    port_config.task_stack =
        APP_TASK_STACK_LVGL;

    port_config.task_affinity =
        APP_TASK_CORE_SERVICES;

    port_config.task_max_sleep_ms =
        250;

    esp_err_t err =
        lvgl_port_init(
            &port_config
        );

    if (err != ESP_OK) {
        return err;
    }

    const lvgl_port_display_cfg_t display_config = {
        .io_handle =
            display_io_handle(),

        .panel_handle =
            display_panel_handle(),

        .buffer_size =
            DISPLAY_WIDTH *
            UI_PERF_FLUSH_CHUNK_ROWS,

        .double_buffer =
            false,

        .trans_size =
            0,

        .hres =
            DISPLAY_WIDTH,

        .vres =
            DISPLAY_HEIGHT,

        .monochrome =
            false,

        .color_format =
            LV_COLOR_FORMAT_RGB565,

        /*
         * The physical ILI9341 is native 240x320. esp_lvgl_port
         * owns the one and only rotation decision and applies the
         * verified 320x240 landscape orientation here.
         */
        .rotation = {
            .swap_xy = true,
            .mirror_x = false,
            .mirror_y = false,
        },

        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
            .swap_bytes = true,
            .full_refresh = false,
            .direct_mode = false,
        },
    };

    context->display =
        lvgl_port_add_disp(
            &display_config
        );

    if (context->display == NULL) {
        (void)lvgl_port_deinit();

        return ESP_FAIL;
    }

    if (lv_display_add_event_cb(context->display, ui_lvgl_perf_event, LV_EVENT_ALL, NULL) == NULL) {
        ESP_LOGW(TAG, "Unable to register LVGL render and flush timing hooks");
    }

    context->port_ready =
        true;

    err =
        app_spi_bus_register_display_guard(
            ui_lvgl_quiesce_shared_spi,
            ui_lvgl_resume_shared_spi,
            NULL
        );

    if (err != ESP_OK) {
        ui_lvgl_deinit();
        return err;
    }

    ESP_LOGI(
        TAG,
        "LVGL 9 display ready: %dx%d landscape, Core %d, RGB565 %d-row single DMA buffer",
        DISPLAY_WIDTH,
        DISPLAY_HEIGHT,
        APP_TASK_CORE_SERVICES,
        UI_PERF_FLUSH_CHUNK_ROWS
    );

    ESP_LOGI(
        TAG,
        "DMA_RUNTIME owner=tft location=internal bytes=%u mode=lvgl_dma_buffer",
        (unsigned)(DISPLAY_WIDTH * UI_PERF_FLUSH_CHUNK_ROWS * sizeof(uint16_t))
    );

    ui_lvgl_log_memory_speed();

    return ESP_OK;
}


void ui_lvgl_deinit(void)
{
    ui_context_t *context =
        ui_context();

    if (!context->port_ready) {
        return;
    }

    /*
     * Wait for an active SD operation to leave the shared-bus guard before
     * tearing down LVGL. After this point no new storage operation will try
     * to lock a deinitialized LVGL mutex.
     */
    app_spi_bus_unregister_display_guard();

    if (context->display != NULL) {
        if (!lvgl_port_lock(0)) {
            ESP_LOGW(
                TAG,
                "Unable to lock LVGL while draining TFT DMA"
            );
        }
        else {
            const esp_err_t drain_err =
                ui_lvgl_drain_panel_io_locked();
            if (drain_err != ESP_OK) {
                ESP_LOGW(
                    TAG,
                    "Failed to drain TFT DMA before LVGL teardown: %s",
                    esp_err_to_name(drain_err)
                );
            }
            lvgl_port_unlock();
        }
    }

    if (context->display != NULL) {
        (void)lvgl_port_remove_disp(
            context->display
        );

        context->display =
            NULL;
    }

    (void)lvgl_port_deinit();

    context->port_ready =
        false;

    context->root =
        NULL;

    ESP_LOGI(
        TAG,
        "LVGL deinitialized"
    );
}
