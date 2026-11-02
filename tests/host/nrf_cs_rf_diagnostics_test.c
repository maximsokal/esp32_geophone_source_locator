#include <assert.h>
#include <stdint.h>
#include "nrf_cs_rf_diagnostics.h"

int main(void)
{
    nrf_cs_rf_diagnostics_t d;
    nrf_cs_rf_reset(&d);
    assert(nrf_cs_rf_signal_word(&d, NRF_CS_RF_UNAVAILABLE) == UINT32_C(0x7f7f7f7f));
    uint32_t q = nrf_cs_rf_quality_word(&d, 0, 0);
    assert((q & 127U) == 127U && ((q >> 7) & 127U) == 127U);
    assert(((q >> 14) & 127U) == 127U);
    nrf_cs_rf_packet(&d.local, -90, 0);
    nrf_cs_rf_packet(&d.local, -50, 1);
    nrf_cs_rf_packet(&d.local, 127, 0); /* unavailable is not +127 dBm */
    nrf_cs_rf_packet(&d.peer, -80, 0);
    d.peer_tx_dbm = -8;
    nrf_cs_rf_tone(&d.local, 0);
    nrf_cs_rf_tone(&d.local, 2);
    nrf_cs_rf_tone(&d.peer, 0);
    d.good_channels = 15;
    uint32_t signal = nrf_cs_rf_signal_word(&d, 4);
    assert((int8_t)signal == -70);
    assert((int8_t)(signal >> 8) == -80);
    assert((int8_t)(signal >> 16) == 4);
    assert((int8_t)(signal >> 24) == -8);
    q = nrf_cs_rf_quality_word(&d, 1, NRF_CS_RF_METADATA);
    assert((q & 127U) == 50U && ((q >> 7) & 127U) == 0U);
    assert(((q >> 14) & 127U) == 15U && ((q >> 21) & 15U) == 1U);
    assert((q >> 25) & NRF_CS_RF_LOCAL_PACKET_ERROR);
    assert(!((q >> 25) & NRF_CS_RF_ANTENNA_MISMATCH));
    assert((nrf_cs_rf_quality_word(&d, 2, 0) >> 25) & NRF_CS_RF_ANTENNA_MISMATCH);
    nrf_cs_rf_packet(&d.peer, 100, 2); /* invalid RSSI is not averaged */
    assert((int8_t)(nrf_cs_rf_signal_word(&d, 127) >> 8) == -80);
    for (unsigned i = 0; i < 70000; ++i) nrf_cs_rf_packet(&d.local, -70, 0);
    assert(d.local.rssi_count == UINT16_MAX);
    assert((int8_t)nrf_cs_rf_signal_word(&d, 127) == -70);
    uint32_t status = nrf_cs_rf_status_word(-65, 4, 1, 2, 3, true);
    assert((status & 15U) == 1U && ((status >> 4) & 15U) == 2U);
    assert(((status >> 8) & 15U) == 3U);
    assert((int8_t)(status >> 12) == -65 && (int8_t)(status >> 20) == 4);
    assert(status >> 31);
    return 0;
}
