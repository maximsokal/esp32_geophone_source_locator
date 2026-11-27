#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PB2_PRU_RING_MAGIC       0x50425231U /* 'PBR1' */
#define PB2_PRU_RING_VERSION     1U
#define PB2_PRU_RING_CAPACITY    128U /* power of 2, fits cleanly in 8 KB PRU_DMEM1 */
#define PB2_PRU_CHANNEL_COUNT    4U

#define PB2_PRU_PROFILE_SINGLE_DOUT 0U /* V1..V4 on DOUTA (64 SCLK) */
#define PB2_PRU_PROFILE_DUAL_DOUT   1U /* V1,V2 on DOUTA + V5,V6 on DOUTB (32 SCLK) */

#define PB2_PRU_ERR_BUSY_STUCK_HIGH 0x0001U
#define PB2_PRU_ERR_BUSY_STUCK_LOW  0x0002U
#define PB2_PRU_ERR_OVERRUN         0x0004U

typedef struct {
    uint64_t source_sample_index;
    uint64_t pru_tick_counter;
    uint32_t sequence;
    uint16_t flags;
    uint16_t reserved;
    int16_t channels[PB2_PRU_CHANNEL_COUNT]; /* logical geophone order [1..4] */
} pb2_pru_sample_frame_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t channel_count;
    uint8_t profile;
    uint32_t boot_epoch;
    uint32_t session_epoch;
    uint32_t sample_rate_hz;
    uint8_t channel_map[PB2_PRU_CHANNEL_COUNT];
    volatile uint32_t write_idx;
    volatile uint32_t read_idx;
    volatile uint64_t dropped_frames;
    volatile uint64_t overrun_count;
    volatile uint32_t hw_error_flags;
    pb2_pru_sample_frame_t slots[PB2_PRU_RING_CAPACITY];
} pb2_pru_ring_t;

#ifdef __cplusplus
}
#endif
