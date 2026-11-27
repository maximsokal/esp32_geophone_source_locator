/*
 * AM6232 PRU1 AD7606 4-channel deterministic driver (Phase 4).
 * Compiles with TI PRU CGT (clpru) + PSSP against AM6232_PRU1.cmd.
 *
 * Pin mapping (PocketBeagle 2 Rev A0, PRU1):
 *   R30.2  (P2.06) -> CONVST A+B
 *   R30.3  (P2.08) -> RESET
 *   R30.12 (P2.30) -> CS_N
 *   R30.11 (P2.32) -> RD/SCLK
 *   R31.4  (P2.10) <- BUSY
 *   R31.6  (P2.24) <- DOUTA (DB7)
 *   R31.7  (P2.33) <- DOUTB (DB8)
 */

#include <stdint.h>
#include "pru_adc_shared.h"

volatile register uint32_t __R30;
volatile register uint32_t __R31;

#define PIN_CONVST  (1U << 2)
#define PIN_RESET   (1U << 3)
#define PIN_SCLK    (1U << 11)
#define PIN_CS_N    (1U << 12)

#define PIN_BUSY    (1U << 4)
#define PIN_DOUTA   (1U << 6)
#define PIN_DOUTB   (1U << 7)

static inline void pru_delay_cycles(uint32_t n) {
    volatile uint32_t i;
    for (i = 0U; i < n; ++i) {
    }
}

/* Read 32 SCLK cycles simultaneously from DOUTA (V1,V2) and DOUTB (V5,V6)
 * into logical channels [0..3]. Must complete >= t6 (25 ns) before BUSY falls. */
static void read_dual_dout_32(int16_t out_ch[PB2_PRU_CHANNEL_COUNT]) {
    uint16_t a_hi = 0U, a_lo = 0U, b_hi = 0U, b_lo = 0U;
    uint32_t bit;

    __R30 &= ~PIN_CS_N;
    for (bit = 0U; bit < 16U; ++bit) {
        __R30 &= ~PIN_SCLK;
        pru_delay_cycles(2U);
        const uint32_t r31 = __R31;
        a_hi = (uint16_t)((a_hi << 1U) | ((r31 & PIN_DOUTA) ? 1U : 0U));
        b_hi = (uint16_t)((b_hi << 1U) | ((r31 & PIN_DOUTB) ? 1U : 0U));
        __R30 |= PIN_SCLK;
        pru_delay_cycles(2U);
    }
    for (bit = 0U; bit < 16U; ++bit) {
        __R30 &= ~PIN_SCLK;
        pru_delay_cycles(2U);
        const uint32_t r31 = __R31;
        a_lo = (uint16_t)((a_lo << 1U) | ((r31 & PIN_DOUTA) ? 1U : 0U));
        b_lo = (uint16_t)((b_lo << 1U) | ((r31 & PIN_DOUTB) ? 1U : 0U));
        __R30 |= PIN_SCLK;
        pru_delay_cycles(2U);
    }
    __R30 |= PIN_CS_N;

    out_ch[0] = (int16_t)a_hi;
    out_ch[1] = (int16_t)a_lo;
    out_ch[2] = (int16_t)b_hi;
    out_ch[3] = (int16_t)b_lo;
}

/* Read 64 SCLK cycles from DOUTA (V1..V4). */
static void read_single_dout_64(int16_t out_ch[PB2_PRU_CHANNEL_COUNT]) {
    uint32_t ch, bit;
    __R30 &= ~PIN_CS_N;
    for (ch = 0U; ch < PB2_PRU_CHANNEL_COUNT; ++ch) {
        uint16_t word = 0U;
        for (bit = 0U; bit < 16U; ++bit) {
            __R30 &= ~PIN_SCLK;
            pru_delay_cycles(2U);
            word = (uint16_t)((word << 1U) | ((__R31 & PIN_DOUTA) ? 1U : 0U));
            __R30 |= PIN_SCLK;
            pru_delay_cycles(2U);
        }
        out_ch[ch] = (int16_t)word;
    }
    __R30 |= PIN_CS_N;
}

void pru1_acquire_loop(volatile pb2_pru_ring_t *ring, uint32_t max_iterations) {
    uint64_t sample_idx = 0ULL;
    uint32_t seq = 0U;
    uint32_t iter = 0U;

    __R30 |= (PIN_CS_N | PIN_SCLK | PIN_CONVST);
    __R30 &= ~PIN_RESET;

    while (max_iterations == 0U || iter < max_iterations) {
        /* Start conversion: CONVST low >= 25 ns then rising edge */
        __R30 &= ~PIN_CONVST;
        pru_delay_cycles(4U);
        __R30 |= PIN_CONVST;

        /* Wait for BUSY high (t1 <= 45 ns) */
        uint32_t wait = 0U;
        while ((__R31 & PIN_BUSY) == 0U) {
            if (++wait > 1000U) {
                ring->hw_error_flags |= PB2_PRU_ERR_BUSY_STUCK_LOW;
                return;
            }
        }

        int16_t ch[PB2_PRU_CHANNEL_COUNT];
        if (ring->profile == PB2_PRU_PROFILE_DUAL_DOUT) {
            read_dual_dout_32(ch);
        } else {
            read_single_dout_64(ch);
        }

        /* Wait for BUSY falling edge */
        wait = 0U;
        while ((__R31 & PIN_BUSY) != 0U) {
            if (++wait > 50000U) {
                ring->hw_error_flags |= PB2_PRU_ERR_BUSY_STUCK_HIGH;
                return;
            }
        }

        const uint32_t next_w = (ring->write_idx + 1U) & (PB2_PRU_RING_CAPACITY - 1U);
        if (next_w == ring->read_idx) {
            ring->overrun_count++;
            ring->dropped_frames++;
            ring->hw_error_flags |= PB2_PRU_ERR_OVERRUN;
        } else {
            volatile pb2_pru_sample_frame_t *slot = &ring->slots[ring->write_idx];
            slot->source_sample_index = sample_idx;
            slot->pru_tick_counter = sample_idx * 1000ULL;
            slot->sequence = seq;
            slot->flags = 0U;
            slot->channels[0] = ch[0];
            slot->channels[1] = ch[1];
            slot->channels[2] = ch[2];
            slot->channels[3] = ch[3];
            ring->write_idx = next_w;
        }

        ++sample_idx;
        ++seq;
        ++iter;
    }
}
