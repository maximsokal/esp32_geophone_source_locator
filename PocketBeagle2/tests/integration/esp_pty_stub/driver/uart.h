#pragma once

#include "esp_err.h"

#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UART_NUM_2 2
#define UART_DATA_8_BITS 3
#define UART_PARITY_DISABLE 0
#define UART_STOP_BITS_1 1
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_DEFAULT 0
#define UART_PIN_NO_CHANGE (-1)

typedef struct {
    int baud_rate;
    int data_bits;
    int parity;
    int stop_bits;
    int flow_ctrl;
    int source_clk;
} uart_config_t;

extern int g_esp_pty_fd;
extern size_t g_esp_tx_free_bytes;

static inline esp_err_t uart_param_config(int port, const uart_config_t* cfg) {
    (void)port;
    (void)cfg;
    return ESP_OK;
}

static inline esp_err_t uart_set_pin(int port, int tx, int rx, int rts, int cts) {
    (void)port; (void)tx; (void)rx; (void)rts; (void)cts;
    return ESP_OK;
}

static inline esp_err_t uart_driver_install(int port, int rx_sz, int tx_sz, int q_sz, void* q, int flags) {
    (void)port; (void)rx_sz; (void)tx_sz; (void)q_sz; (void)q; (void)flags;
    return ESP_OK;
}

static inline esp_err_t uart_get_tx_buffer_free_size(int port, size_t* size) {
    (void)port;
    if (size) {
        *size = g_esp_tx_free_bytes;
    }
    return ESP_OK;
}

static inline int uart_write_bytes(int port, const void* src, size_t size) {
    (void)port;
    if (g_esp_pty_fd < 0 || !src) {
        return -1;
    }
    return (int)write(g_esp_pty_fd, src, size);
}

#ifdef __cplusplus
}
#endif
