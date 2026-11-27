#include "pb2_uart_bridge.h"

#include "pb2_protocol.h"
#include "seismic_buffer.h"

#include "sdkconfig.h"

#if defined(CONFIG_PB2_UART_BRIDGE_ENABLE) && CONFIG_PB2_UART_BRIDGE_ENABLE
#include "driver/uart.h"
#include "esp_log.h"
#endif

#include <string.h>

#if defined(CONFIG_PB2_UART_BRIDGE_ENABLE) && CONFIG_PB2_UART_BRIDGE_ENABLE

static const char *TAG = "PB2_UART";

#define PB2_ESP_UART_PORT   UART_NUM_2
#define PB2_ESP_UART_TX_PIN 7
#if defined(CONFIG_PB2_UART_PROFILE_DEVKITC_V1_0) && CONFIG_PB2_UART_PROFILE_DEVKITC_V1_0
#define PB2_ESP_UART_RX_PIN 38
#else
#define PB2_ESP_UART_RX_PIN 48
#endif

static bool s_initialized = false;
static uint64_t s_cursor = 0ULL;
static uint32_t s_seq = 1U;
static uint32_t s_session_epoch = 1U;
static pb2_uart_bridge_stats_t s_stats = {0};

esp_err_t pb2_uart_bridge_init(void) {
    if (s_initialized) {
        return ESP_OK;
    }

    const uart_config_t cfg = {
        .baud_rate = CONFIG_PB2_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(PB2_ESP_UART_PORT, &cfg);
    if (err != ESP_OK) return err;

    err = uart_set_pin(PB2_ESP_UART_PORT, PB2_ESP_UART_TX_PIN, PB2_ESP_UART_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;

    err = uart_driver_install(PB2_ESP_UART_PORT, 1024, 2048, 0, NULL, 0);
    if (err != ESP_OK) return err;

    s_stats.enabled = true;
    s_stats.baud_rate = CONFIG_PB2_UART_BAUD_RATE;
    s_stats.session_epoch = s_session_epoch;
    s_initialized = true;
    ESP_LOGI(TAG, "UART2 bridge ready: TX=%d RX=%d baud=%d",
             PB2_ESP_UART_TX_PIN, PB2_ESP_UART_RX_PIN, CONFIG_PB2_UART_BAUD_RATE);
    return ESP_OK;
}

esp_err_t pb2_uart_bridge_poll_once(void) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    seismic_frame_t frames[PB2_PROTO_PREVIEW_MAX_FRAMES];
    uint64_t indices[PB2_PROTO_PREVIEW_MAX_FRAMES];
    size_t count = 0U;
    uint64_t dropped = 0ULL;
    seismic_buffer_info_t info = {0};

    const esp_err_t err = seismic_buffer_copy_since_with_source_indices(
        &s_cursor, frames, indices, PB2_PROTO_PREVIEW_MAX_FRAMES, &count, &dropped, &info
    );

    if (err == ESP_ERR_INVALID_STATE) {
        /* Acquisition generation changed: reset cursor and advance session epoch. */
        s_cursor = 0ULL;
        ++s_session_epoch;
        s_stats.session_epoch = s_session_epoch;
        return ESP_OK;
    }
    if (err != ESP_OK || count == 0U) {
        return err;
    }

    if (dropped > 0ULL) {
        s_stats.buffer_gaps += dropped;
    }

    const uint32_t decimation = (info.sample_rate_hz > CONFIG_PB2_UART_PREVIEW_TARGET_HZ)
        ? (info.sample_rate_hz / CONFIG_PB2_UART_PREVIEW_TARGET_HZ)
        : 1U;

    pb2_preview_block_t blk;
    memset(&blk, 0, sizeof(blk));
    blk.first_sample_index = indices[0];
    blk.first_timestamp_us = (uint64_t)frames[0].timestamp_us;
    blk.source_sample_rate_hz = info.sample_rate_hz;
    blk.decimation_factor = (uint16_t)decimation;
    blk.block_flags = (dropped > 0ULL) ? PB2_PREVIEW_FLAG_GAP_BEFORE : 0U;
    blk.clock_domain = PB2_CLOCK_ESP32_US;
    blk.sample_format = PB2_SAMPLE_FMT_FLOAT32_UV;
    blk.channel_count = PB2_PROTO_CHANNEL_COUNT;
    for (size_t ch = 0U; ch < PB2_PROTO_CHANNEL_COUNT; ++ch) {
        blk.channel_map[ch] = (uint8_t)(ch + 1U);
    }

    uint16_t out_frames = 0U;
    for (size_t i = 0U; i < count; i += decimation) {
        for (size_t ch = 0U; ch < PB2_PROTO_CHANNEL_COUNT; ++ch) {
            blk.samples_uv[out_frames][ch] = frames[i].channel_uv[ch];
        }
        ++out_frames;
    }
    blk.frame_count = out_frames;
    if (out_frames == 0U) {
        return ESP_OK;
    }

    uint8_t payload[PB2_PROTO_MAX_PAYLOAD];
    uint16_t plen = 0U;
    if (pb2_encode_preview_block(&blk, payload, sizeof(payload), &plen) != PB2_PROTO_OK) {
        return ESP_FAIL;
    }

    const pb2_frame_header_t hdr = {
        .version = PB2_PROTO_VERSION,
        .msg_type = PB2_MSG_PREVIEW_BLOCK,
        .flags = 0U,
        .sequence = s_seq++,
        .boot_epoch = 1U,
        .session_epoch = s_session_epoch,
        .payload_len = plen
    };

    uint8_t wire[PB2_PROTO_MAX_COBS_BYTES];
    size_t wire_len = 0U;
    if (pb2_wire_encode(&hdr, payload, wire, sizeof(wire), &wire_len) != PB2_PROTO_OK) {
        return ESP_FAIL;
    }

    size_t tx_free = 0U;
    if (uart_get_tx_buffer_free_size(PB2_ESP_UART_PORT, &tx_free) != ESP_OK || tx_free < wire_len) {
        ++s_stats.tx_drops;
        return ESP_OK; /* Non-blocking: drop preview rather than stalling acquisition */
    }

    if (uart_write_bytes(PB2_ESP_UART_PORT, wire, wire_len) == (int)wire_len) {
        ++s_stats.preview_blocks_tx;
        s_stats.preview_frames_tx += out_frames;
    } else {
        ++s_stats.tx_drops;
    }
    return ESP_OK;
}

void pb2_uart_bridge_get_stats(pb2_uart_bridge_stats_t *out_stats) {
    if (out_stats != NULL) {
        *out_stats = s_stats;
    }
}

#else

esp_err_t pb2_uart_bridge_init(void) {
    return ESP_OK;
}

esp_err_t pb2_uart_bridge_poll_once(void) {
    return ESP_OK;
}

void pb2_uart_bridge_get_stats(pb2_uart_bridge_stats_t *out_stats) {
    if (out_stats != NULL) {
        memset(out_stats, 0, sizeof(*out_stats));
    }
}

#endif
