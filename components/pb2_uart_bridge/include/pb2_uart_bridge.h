#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool enabled;
    uint32_t baud_rate;
    uint32_t session_epoch;
    uint64_t preview_blocks_tx;
    uint64_t preview_frames_tx;
    uint64_t tx_drops;
    uint64_t buffer_gaps;
} pb2_uart_bridge_stats_t;

esp_err_t pb2_uart_bridge_init(void);
esp_err_t pb2_uart_bridge_poll_once(void);
void pb2_uart_bridge_get_stats(pb2_uart_bridge_stats_t *out_stats);

#ifdef __cplusplus
}
#endif
