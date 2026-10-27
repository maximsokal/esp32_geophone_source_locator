#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { uint32_t tx_amount; uint32_t rx_amount; uint8_t *rx_buffer; } NRF_TWIS_Type;
typedef struct { NRF_TWIS_Type *p_reg; } nrfx_twis_t;
typedef struct { unsigned unused; } nrfx_twis_config_t;
typedef enum {
    NRFX_TWIS_EVT_READ_REQ,
    NRFX_TWIS_EVT_READ_DONE,
    NRFX_TWIS_EVT_READ_ERROR,
    NRFX_TWIS_EVT_WRITE_REQ,
    NRFX_TWIS_EVT_WRITE_DONE,
    NRFX_TWIS_EVT_WRITE_ERROR,
    NRFX_TWIS_EVT_GENERAL_ERROR,
} nrfx_twis_event_type_t;
typedef struct {
    nrfx_twis_event_type_t type;
    union { bool buf_req; uint32_t tx_amount; uint32_t error; } data;
} nrfx_twis_event_t;
typedef void (*nrfx_twis_event_handler_t)(const nrfx_twis_event_t *event);

static inline size_t nrfx_twis_tx_amount(const nrfx_twis_t *instance)
{
    return instance->p_reg->tx_amount;
}

static inline size_t nrfx_twis_rx_amount(const nrfx_twis_t *instance)
{
    return instance->p_reg->rx_amount;
}

static inline uint8_t *nrf_twis_rx_buffer_get(const NRF_TWIS_Type *p_reg)
{
    return p_reg->rx_buffer;
}
