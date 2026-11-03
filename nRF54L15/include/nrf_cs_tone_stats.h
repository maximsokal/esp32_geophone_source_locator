#ifndef NRF_CS_TONE_STATS_H
#define NRF_CS_TONE_STATS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * How complete the frequency set of one Channel Sounding procedure is (CS-IFFT-GAP-01).
 *
 * The IFFT of the Nordic distance library runs over one complex value per channel. A channel whose tone was not good
 * enough on both sides is left out of the sum and its slot stays zero. Several zeros in a row change the shape of the IFFT
 * response and can create a side lobe that the peak search takes for the shortest path: a stable but wrong distance
 * (reported on Nordic DevZone for NCS v3.4.0, 1 m read as 0.23 m). Whether this is what happens on this hardware is not
 * known; this is the number that tells, per attempt, how many channels were usable and how long the longest hole was.
 */

typedef struct {
    uint8_t planned;        /* channels in the channel map of the configuration */
    uint8_t usable;         /* planned channels with a tone good on both sides */
    uint8_t longest_gap;    /* longest run of consecutive slots without a usable tone (excluded channels count: they are zeros too) */
    uint8_t lead_gap;       /* slots without a usable tone at the start of the array (2404 MHz upwards): a Wi-Fi channel 1 access point lands here */
    uint8_t tail_gap;       /* the same at the end of the array (2478 MHz downwards) */
} nrf_cs_tone_stats_t;

static inline nrf_cs_tone_stats_t nrf_cs_tone_stats(const bool *planned, const bool *usable, size_t count)
{
    nrf_cs_tone_stats_t stats = {0U, 0U, 0U, 0U, 0U};
    uint8_t run = 0U;
    bool seen_usable = false;

    if (planned == NULL || usable == NULL || count > 255U) return stats;
    for (size_t i = 0U; i < count; ++i) {
        if (planned[i]) ++stats.planned;
        if (planned[i] && usable[i]) {
            ++stats.usable;
            run = 0U;
            seen_usable = true;
        } else {
            ++run;
            if (run > stats.longest_gap) stats.longest_gap = run;
            if (!seen_usable) stats.lead_gap = run;
        }
    }
    stats.tail_gap = run;       /* the run that is still open at the end of the array */
    return stats;
}

#endif
