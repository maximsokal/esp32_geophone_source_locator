#ifndef NRF_CS_RF_DIAGNOSTICS_H
#define NRF_CS_RF_DIAGNOSTICS_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define NRF_CS_RF_UNAVAILABLE 127
#define NRF_CS_RF_ANTENNA_MISMATCH (1U << 0)
#define NRF_CS_RF_ABORTED (1U << 1)
#define NRF_CS_RF_LOCAL_PACKET_ERROR (1U << 2)
#define NRF_CS_RF_PEER_PACKET_ERROR (1U << 3)
#define NRF_CS_RF_MALFORMED (1U << 4)
#define NRF_CS_RF_METADATA (1U << 5)

typedef struct {
    int32_t rssi_sum;
    uint16_t rssi_count;
    uint16_t tones_good;
    uint16_t tones_bad;
    bool packet_error;
} nrf_cs_rf_side_t;

typedef struct {
    nrf_cs_rf_side_t local;
    nrf_cs_rf_side_t peer;
    int8_t peer_tx_dbm;
    uint8_t good_channels;
    uint8_t flags;
} nrf_cs_rf_diagnostics_t;

static inline void nrf_cs_rf_reset(nrf_cs_rf_diagnostics_t *d)
{
    memset(d, 0, sizeof(*d));
    d->peer_tx_dbm = NRF_CS_RF_UNAVAILABLE;
    d->good_channels = NRF_CS_RF_UNAVAILABLE;
}

static inline void nrf_cs_rf_packet(nrf_cs_rf_side_t *side, int8_t rssi, uint8_t aa_check)
{
    if (aa_check != 0U) side->packet_error = true;
    if (rssi >= -127 && rssi <= 20 && side->rssi_count < UINT16_MAX) {
        side->rssi_sum += rssi;
        ++side->rssi_count;
    }
}

static inline void nrf_cs_rf_tone(nrf_cs_rf_side_t *side, uint8_t quality)
{
    uint16_t *count = quality == 0U ? &side->tones_good : &side->tones_bad;
    if (*count < UINT16_MAX) ++*count;
}

static inline uint8_t nrf_cs_rf_bad_pct(const nrf_cs_rf_side_t *side)
{
    const uint32_t total = (uint32_t)side->tones_good + side->tones_bad;
    return total == 0U ? NRF_CS_RF_UNAVAILABLE : (uint8_t)((uint32_t)side->tones_bad * 100U / total);
}

static inline int8_t nrf_cs_rf_mean_rssi(const nrf_cs_rf_side_t *side)
{
    return side->rssi_count == 0U ? NRF_CS_RF_UNAVAILABLE :
        (int8_t)(side->rssi_sum / (int32_t)side->rssi_count);
}

/* Byte order: local RSSI, peer RSSI, local TX, peer TX; signed dBm, 127 unavailable. */
static inline uint32_t nrf_cs_rf_signal_word(const nrf_cs_rf_diagnostics_t *d, int8_t local_tx)
{
    return (uint8_t)nrf_cs_rf_mean_rssi(&d->local) |
        ((uint32_t)(uint8_t)nrf_cs_rf_mean_rssi(&d->peer) << 8U) |
        ((uint32_t)(uint8_t)local_tx << 16U) | ((uint32_t)(uint8_t)d->peer_tx_dbm << 24U);
}

static inline uint32_t nrf_cs_rf_quality_word(const nrf_cs_rf_diagnostics_t *d,
    uint8_t paths, uint8_t flags)
{
    flags |= d->flags;
    if (paths > 1U) flags |= NRF_CS_RF_ANTENNA_MISMATCH;
    if (d->local.packet_error) flags |= NRF_CS_RF_LOCAL_PACKET_ERROR;
    if (d->peer.packet_error) flags |= NRF_CS_RF_PEER_PACKET_ERROR;
    return nrf_cs_rf_bad_pct(&d->local) | ((uint32_t)nrf_cs_rf_bad_pct(&d->peer) << 7U) |
        ((uint32_t)(d->good_channels & 127U) << 14U) | ((uint32_t)(paths & 15U) << 21U) |
        ((uint32_t)(flags & 127U) << 25U);
}

static inline uint32_t nrf_cs_rf_status_word(int8_t reference_power, int8_t tx,
    uint8_t paths, uint8_t procedure_abort, uint8_t subevent_abort, bool reflector)
{
    return (paths & 15U) | ((uint32_t)(procedure_abort & 15U) << 4U) |
        ((uint32_t)(subevent_abort & 15U) << 8U) |
        ((uint32_t)(uint8_t)reference_power << 12U) |
        ((uint32_t)(uint8_t)tx << 20U) | (reflector ? UINT32_C(0x80000000) : 0U);
}

#endif
