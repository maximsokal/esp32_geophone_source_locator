/*
 * AM6232 PRU1 AD7606 4-channel deterministic driver (Stage A fixed).
 * Compiles with TI PRU CGT (clpru) against AM6232_PRU1.cmd.
 *
 * Uses AM62x PRU-SS IEP0 hardware timer (0x0002E000) for fixed-period CONVST
 * scheduling and 64-bit hardware timestamps, __delay_cycles() for deterministic
 * SCLK timing, pipeline priming/draining for Read During Conversion, and an
 * explicit FSM (IDLE -> CONFIGURED -> ARMED -> ACQUIRING -> DRAINING -> STOPPED/FAULT).
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

#define IEP_BASE_ADDR       0x0002E000U
#define IEP_GLOBAL_CFG      (*(volatile uint32_t *)(IEP_BASE_ADDR + 0x00U))
#define IEP_COUNT_LO        (*(volatile uint32_t *)(IEP_BASE_ADDR + 0x10U))
#define IEP_COUNT_HI        (*(volatile uint32_t *)(IEP_BASE_ADDR + 0x14U))

static uint64_t iep_read_64(void) {
    uint32_t hi1, lo, hi2;
    do {
        hi1 = IEP_COUNT_HI;
        lo = IEP_COUNT_LO;
        hi2 = IEP_COUNT_HI;
    } while (hi1 != hi2);
    return ((uint64_t)hi1 << 32U) | (uint64_t)lo;
}

static void iep_init(void) {
    /* Enable counter with DEFAULT_INC = 1 (5 ns/tick @ 200 MHz IEP clock). */
    IEP_GLOBAL_CFG = 0x00000011U;
}

static void ad7606_hw_reset(void) {
    __R30 |= (PIN_CS_N | PIN_SCLK | PIN_CONVST);
    __R30 |= PIN_RESET;
    __delay_cycles(20); /* 100 ns @ 200 MHz (tRESET >= 50 ns) */
    __R30 &= ~PIN_RESET;
    __delay_cycles(8);  /* 40 ns >= t7 (25 ns) before CONVST */
}

extern void read_dual_dout_32_asm(int16_t out_ch[PB2_PRU_CHANNEL_COUNT]);

/* Deterministic 64-clock single-DOUT readout (V1..V4 on DOUTA) <= 12.5 MHz. */
static void read_single_dout_64(int16_t out_ch[PB2_PRU_CHANNEL_COUNT]) {
    uint32_t ch, bit;
    __R30 &= ~PIN_CS_N;
    __delay_cycles(7);
    for (ch = 0U; ch < PB2_PRU_CHANNEL_COUNT; ++ch) {
        uint16_t word = 0U;
#pragma MUST_ITERATE(16, 16)
#pragma UNROLL(16)
        for (bit = 0U; bit < 16U; ++bit) {
            __R30 &= ~PIN_SCLK;
            __delay_cycles(7);
            __R30 |= PIN_SCLK;
            __delay_cycles(5);
            word = (uint16_t)((word << 1U) | ((__R31 >> 6U) & 1U));
        }
        out_ch[ch] = (int16_t)word;
    }
    __delay_cycles(2);
    __R30 |= PIN_CS_N;
}

static void read_selected_profile(uint8_t profile, int16_t out_ch[PB2_PRU_CHANNEL_COUNT]) {
    if (profile == PB2_PRU_PROFILE_DUAL_DOUT) {
        read_dual_dout_32_asm(out_ch);
    } else {
        read_single_dout_64(out_ch);
    }
}

typedef struct {
    volatile pb2_pru_block_t *active_blk;
    uint32_t w_idx;
    uint16_t frame_count;
    uint32_t seq;
    uint8_t gap_pending;
    uint8_t jitter_pending;
} block_acc_t;

static void flush_block(volatile pb2_pru_shared_t *shm, block_acc_t *acc) {
    if (acc->active_blk == 0 || acc->frame_count == 0U) {
        acc->active_blk = 0;
        acc->frame_count = 0U;
        return;
    }
    acc->active_blk->frame_count = acc->frame_count;
    shm->write_block_idx = (acc->w_idx + 1U) & (PB2_PRU_BLOCK_CAPACITY - 1U);
    acc->active_blk = 0;
    acc->frame_count = 0U;
}

static void push_sample(
    volatile pb2_pru_shared_t *shm,
    block_acc_t *acc,
    uint64_t sample_idx,
    uint64_t iep_tick,
    const int16_t ch[PB2_PRU_CHANNEL_COUNT]
) {
    if (acc->active_blk == 0) {
        const uint32_t w = shm->write_block_idx;
        const uint32_t next_w = (w + 1U) & (PB2_PRU_BLOCK_CAPACITY - 1U);
        if (next_w == shm->read_block_idx) {
            shm->overrun_count++;
            shm->dropped_frames++;
            shm->hw_error_flags |= PB2_PRU_ERR_OVERRUN;
            acc->gap_pending = 1U;
            return;
        }
        acc->w_idx = w;
        acc->active_blk = &shm->blocks[w];
        acc->active_blk->first_sample_index = sample_idx;
        acc->active_blk->first_iep_ticks = iep_tick;
        acc->active_blk->sequence = acc->seq++;
        acc->active_blk->frame_count = 0U;
        {
            uint16_t fl = 0U;
            if (acc->gap_pending) {
                fl |= PB2_PRU_BLK_FLAG_GAP_BEFORE;
                acc->gap_pending = 0U;
            }
            if (acc->jitter_pending) {
                fl |= PB2_PRU_BLK_FLAG_JITTER_WARN;
                acc->jitter_pending = 0U;
            }
            acc->active_blk->flags = fl;
        }
        acc->frame_count = 0U;
    }
    {
        const uint16_t idx = acc->frame_count;
        volatile int16_t *dst = acc->active_blk->channels[idx];
        dst[0] = ch[0];
        dst[1] = ch[1];
        dst[2] = ch[2];
        dst[3] = ch[3];
        acc->frame_count = idx + 1U;
    }
    shm->total_frames++;
    if (acc->frame_count >= PB2_PRU_BLOCK_FRAMES) {
        flush_block(shm, acc);
    }
}

static uint8_t config_is_valid(volatile pb2_pru_shared_t *shm) {
    if (shm->adc_owner != PB2_PRU_OWNER_BEAGLE) return 0U;
    if (shm->sample_rate_hz < 500U || shm->sample_rate_hz > 200000U) return 0U;
    if (shm->profile != PB2_PRU_PROFILE_SINGLE_DOUT && shm->profile != PB2_PRU_PROFILE_DUAL_DOUT) return 0U;
    if (shm->profile == PB2_PRU_PROFILE_SINGLE_DOUT && shm->sample_rate_hz > 80000U) return 0U;
    return 1U;
}

static void run_acquisition(volatile pb2_pru_shared_t *shm) {
    const uint32_t iep_hz = (shm->iep_clock_hz != 0U) ? shm->iep_clock_hz : 200000000U;
    const uint64_t period_ticks = (uint64_t)iep_hz / (uint64_t)shm->sample_rate_hz;
    const uint32_t rem_ticks = iep_hz % shm->sample_rate_hz;
    /* Timing contract: only DUAL_DOUT (32 SCLK <= 2.56 us) fits inside tCONV_min (3.45 us)
     * for Read During Conversion; SINGLE_DOUT (64 SCLK >= 5.12 us) MUST use Read After Conversion. */
    const uint8_t use_read_during_conv = (shm->profile == PB2_PRU_PROFILE_DUAL_DOUT) ? 1U : 0U;

    shm->iep_clock_hz = iep_hz;
    shm->iep_period_ticks = (uint32_t)period_ticks;

    block_acc_t acc;
    acc.active_blk = 0;
    acc.w_idx = 0U;
    acc.frame_count = 0U;
    acc.seq = 0U;
    acc.gap_pending = 0U;
    acc.jitter_pending = 0U;

    uint64_t next_convst_tick = iep_read_64() + 100ULL;
    uint32_t rem_acc = 0U;
    uint64_t conv_idx = 0ULL;
    uint64_t prev_conv_idx = 0ULL;
    uint64_t prev_conv_tick = 0ULL;
    uint8_t have_pending_pipeline = 0U;
    uint32_t stop_seq = 0U;
    uint8_t stop_pending = 0U;

    while (shm->state == PB2_PRU_STATE_ACQUIRING) {
        if (shm->cmd == PB2_PRU_CMD_STOP && shm->cmd_seq != shm->last_cmd_ack) {
            stop_seq = shm->cmd_seq;
            stop_pending = 1U;
            shm->state = PB2_PRU_STATE_DRAINING;
            break;
        }

        /* Wait for IEP scheduled CONVST tick (also check STOP so low rates stop immediately) */
        uint64_t now = iep_read_64();
        while (now < next_convst_tick) {
            if (shm->cmd == PB2_PRU_CMD_STOP && shm->cmd_seq != shm->last_cmd_ack) {
                stop_seq = shm->cmd_seq;
                stop_pending = 1U;
                shm->state = PB2_PRU_STATE_DRAINING;
                break;
            }
            now = iep_read_64();
        }
        if (stop_pending) {
            break;
        }
        {
            const uint64_t jitter = now - next_convst_tick;
            if (jitter > (uint64_t)shm->max_jitter_ticks) {
                shm->max_jitter_ticks = (jitter > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t)jitter;
            }
            if (jitter >= period_ticks) {
                const uint64_t skipped_slots = jitter / period_ticks;
                shm->missed_deadlines += skipped_slots;
                shm->hw_error_flags |= PB2_PRU_ERR_MISSED_DEADLINE;
                /* Close current block cleanly and mark gap on next block */
                flush_block(shm, &acc);
                acc.gap_pending = 1U;
                acc.jitter_pending = 1U;
                conv_idx += skipped_slots;
                next_convst_tick += skipped_slots * period_ticks;
            }
        }
        next_convst_tick += period_ticks;
        if (rem_ticks != 0U) {
            rem_acc += rem_ticks;
            if (rem_acc >= shm->sample_rate_hz) {
                rem_acc -= shm->sample_rate_hz;
                next_convst_tick += 1ULL;
            }
        }

        /* Trigger CONVST A+B */
        __R30 &= ~PIN_CONVST;
        __delay_cycles(6); /* t2 >= 25 ns */
        __R30 |= PIN_CONVST;
        const uint64_t convst_tick = iep_read_64();

        /* Wait for BUSY HIGH */
        uint32_t wait = 0U;
        while ((__R31 & PIN_BUSY) == 0U) {
            if (++wait > 1000U) {
                shm->hw_error_flags |= PB2_PRU_ERR_BUSY_STUCK_LOW;
                shm->state = PB2_PRU_STATE_FAULT;
                flush_block(shm, &acc);
                return;
            }
        }

        if (use_read_during_conv) {
            int16_t ch[PB2_PRU_CHANNEL_COUNT];
            if (have_pending_pipeline) {
                read_selected_profile(shm->profile, ch);
                /* Verify BUSY remained HIGH through CS_N rising edge */
                if ((__R31 & PIN_BUSY) == 0U) {
                    shm->hw_error_flags |= PB2_PRU_ERR_BUSY_FELL_IN_READ;
                    shm->dropped_frames++;
                    flush_block(shm, &acc);
                    acc.gap_pending = 1U;
                } else {
                    /* Overlap ring block publish with remaining tCONV */
                    push_sample(shm, &acc, prev_conv_idx, prev_conv_tick, ch);
                }
            }

            wait = 0U;
            while ((__R31 & PIN_BUSY) != 0U) {
                if (++wait > 10000U) {
                    shm->hw_error_flags |= PB2_PRU_ERR_BUSY_STUCK_HIGH;
                    shm->state = PB2_PRU_STATE_FAULT;
                    flush_block(shm, &acc);
                    return;
                }
            }

            prev_conv_idx = conv_idx;
            prev_conv_tick = convst_tick;
            have_pending_pipeline = 1U;
        } else {
            /* Read After Conversion (Fig. 2) */
            wait = 0U;
            while ((__R31 & PIN_BUSY) != 0U) {
                if (++wait > 10000U) {
                    shm->hw_error_flags |= PB2_PRU_ERR_BUSY_STUCK_HIGH;
                    shm->state = PB2_PRU_STATE_FAULT;
                    flush_block(shm, &acc);
                    return;
                }
            }
            int16_t ch[PB2_PRU_CHANNEL_COUNT];
            read_selected_profile(shm->profile, ch);
            push_sample(shm, &acc, conv_idx, convst_tick, ch);
        }

        ++conv_idx;
    }

    /* Pipeline draining on STOP */
    if (shm->state == PB2_PRU_STATE_DRAINING && use_read_during_conv && have_pending_pipeline) {
        uint32_t wait = 0U;
        while ((__R31 & PIN_BUSY) != 0U) {
            if (++wait > 10000U) {
                shm->hw_error_flags |= PB2_PRU_ERR_BUSY_STUCK_HIGH;
                shm->state = PB2_PRU_STATE_FAULT;
                flush_block(shm, &acc);
                return;
            }
        }
        int16_t ch[PB2_PRU_CHANNEL_COUNT];
        read_selected_profile(shm->profile, ch);
        push_sample(shm, &acc, prev_conv_idx, prev_conv_tick, ch);
    }

    flush_block(shm, &acc);
    if (shm->state != PB2_PRU_STATE_FAULT) {
        shm->state = PB2_PRU_STATE_STOPPED;
    }
    if (stop_pending) {
        shm->last_cmd_ack = stop_seq;
    }
}

void pru1_fsm_run(volatile pb2_pru_shared_t *shm) {
    iep_init();

    shm->version = PB2_PRU_RING_VERSION;
    shm->channel_count = PB2_PRU_CHANNEL_COUNT;
    shm->state = PB2_PRU_STATE_IDLE;
    shm->cmd = PB2_PRU_CMD_NONE;
    shm->cmd_seq = 0U;
    shm->last_cmd_ack = 0U;
    shm->write_block_idx = 0U;
    shm->read_block_idx = 0U;
    shm->total_frames = 0ULL;
    shm->dropped_frames = 0ULL;
    shm->overrun_count = 0ULL;
    shm->missed_deadlines = 0ULL;
    shm->max_jitter_ticks = 0U;
    shm->hw_error_flags = 0U;

    /* Safe idle pin levels without pulsing RESET or CONVST */
    __R30 |= (PIN_CS_N | PIN_SCLK | PIN_CONVST);
    __R30 &= ~PIN_RESET;

    /* Publish magic last as initialization-complete sentinel */
    shm->magic = PB2_PRU_RING_MAGIC;

    for (;;) {
        if (shm->cmd_seq != shm->last_cmd_ack) {
            const uint32_t cmd = shm->cmd;
            const uint32_t seq = shm->cmd_seq;

            if (cmd == PB2_PRU_CMD_RESET_FAULT) {
                shm->hw_error_flags = PB2_PRU_ERR_NONE;
                shm->state = PB2_PRU_STATE_IDLE;
                shm->last_cmd_ack = seq;
            } else if (cmd == PB2_PRU_CMD_CONFIGURE &&
                       (shm->state == PB2_PRU_STATE_IDLE || shm->state == PB2_PRU_STATE_STOPPED || shm->state == PB2_PRU_STATE_CONFIGURED)) {
                if (config_is_valid(shm)) {
                    if (shm->profile == PB2_PRU_PROFILE_DUAL_DOUT) {
                        shm->channel_map[0] = 1U;
                        shm->channel_map[1] = 2U;
                        shm->channel_map[2] = 5U;
                        shm->channel_map[3] = 6U;
                    } else {
                        shm->channel_map[0] = 1U;
                        shm->channel_map[1] = 2U;
                        shm->channel_map[2] = 3U;
                        shm->channel_map[3] = 4U;
                    }
                    shm->state = PB2_PRU_STATE_CONFIGURED;
                } else {
                    shm->hw_error_flags |= PB2_PRU_ERR_INVALID_CONFIG;
                    shm->state = PB2_PRU_STATE_FAULT;
                }
                shm->last_cmd_ack = seq;
            } else if (cmd == PB2_PRU_CMD_ARM && shm->state == PB2_PRU_STATE_CONFIGURED) {
                ad7606_hw_reset();
                shm->write_block_idx = 0U;
                shm->read_block_idx = 0U;
                shm->total_frames = 0ULL;
                shm->dropped_frames = 0ULL;
                shm->overrun_count = 0ULL;
                shm->missed_deadlines = 0ULL;
                shm->max_jitter_ticks = 0U;
                shm->hw_error_flags = PB2_PRU_ERR_NONE;
                shm->state = PB2_PRU_STATE_ARMED;
                shm->last_cmd_ack = seq;
            } else if (cmd == PB2_PRU_CMD_START && shm->state == PB2_PRU_STATE_ARMED) {
                shm->state = PB2_PRU_STATE_ACQUIRING;
                shm->last_cmd_ack = seq;
                run_acquisition(shm);
            } else {
                shm->last_cmd_ack = seq;
            }
        }
        __delay_cycles(20);
    }
}
