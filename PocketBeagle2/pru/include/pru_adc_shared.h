#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PB2_PRU_RING_MAGIC        0x50425232U /* 'PBR2' */
#define PB2_PRU_RING_VERSION      2U
#define PB2_PRU_CHANNEL_COUNT     4U
#define PB2_PRU_BLOCK_FRAMES      16U
#define PB2_PRU_BLOCK_CAPACITY    64U /* 64 * 16 = 1024 frames in 32 KB ICSS_SHARED */
#define PB2_PRU_SHARED_MEM_ADDR   0x00010000U /* PRU local ICSS_SHARED base */
#define PB2_PRU_SHARED_MEM_SIZE   0x00008000U /* 32 KB per AM62x TRM & k3-am62-main.dtsi */
#define PB2_AM62X_PRUSS_BASE_ADDR 0x30040000UL /* k3-am62-main.dtsi pruss@30040000 */
#define PB2_AM62X_PRU_SHARED_PHYS 0x30050000UL /* PRUSS base (0x30040000) + shared offset (0x10000) */

#define PB2_PRU_PROFILE_SINGLE_DOUT 0U /* V1..V4 on DOUTA (64 SCLK) */
#define PB2_PRU_PROFILE_DUAL_DOUT   1U /* V1,V2 on DOUTA + V5,V6 on DOUTB (32 SCLK) */

#define PB2_PRU_OWNER_NONE        0U
#define PB2_PRU_OWNER_ESP32       1U
#define PB2_PRU_OWNER_BEAGLE      2U

typedef enum {
    PB2_PRU_STATE_IDLE       = 0,
    PB2_PRU_STATE_CONFIGURED = 1,
    PB2_PRU_STATE_ARMED      = 2,
    PB2_PRU_STATE_ACQUIRING  = 3,
    PB2_PRU_STATE_DRAINING   = 4,
    PB2_PRU_STATE_STOPPED    = 5,
    PB2_PRU_STATE_FAULT      = 6
} pb2_pru_state_t;

typedef enum {
    PB2_PRU_CMD_NONE        = 0,
    PB2_PRU_CMD_CONFIGURE   = 1,
    PB2_PRU_CMD_ARM         = 2,
    PB2_PRU_CMD_START       = 3,
    PB2_PRU_CMD_STOP        = 4,
    PB2_PRU_CMD_RESET_FAULT = 5
} pb2_pru_cmd_t;

#define PB2_PRU_ERR_NONE                 0x0000U
#define PB2_PRU_ERR_BUSY_STUCK_HIGH      0x0001U
#define PB2_PRU_ERR_BUSY_STUCK_LOW       0x0002U
#define PB2_PRU_ERR_OVERRUN              0x0004U
#define PB2_PRU_ERR_BUSY_FELL_IN_READ    0x0008U
#define PB2_PRU_ERR_INVALID_CONFIG       0x0010U
#define PB2_PRU_ERR_MISSED_DEADLINE      0x0020U

#define PB2_PRU_BLK_FLAG_GAP_BEFORE      0x0001U
#define PB2_PRU_BLK_FLAG_JITTER_WARN     0x0002U

typedef struct {
    uint64_t first_sample_index;
    uint64_t first_iep_ticks;
    uint32_t sequence;
    uint16_t frame_count;
    uint16_t flags;
    int16_t channels[PB2_PRU_BLOCK_FRAMES][PB2_PRU_CHANNEL_COUNT];
} pb2_pru_block_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t channel_count;
    uint8_t profile;

    volatile uint32_t state;
    volatile uint32_t cmd;
    volatile uint32_t cmd_seq;
    volatile uint32_t last_cmd_ack;

    uint32_t adc_owner;
    uint32_t sample_rate_hz;
    uint32_t iep_clock_hz;
    uint32_t iep_period_ticks;
    uint32_t boot_epoch;
    uint32_t session_epoch;
    uint8_t channel_map[PB2_PRU_CHANNEL_COUNT];

    volatile uint32_t write_block_idx;
    volatile uint32_t read_block_idx;
    uint32_t reserved_pad0; /* Align uint64_t fields to 8 bytes on both clpru and ARM64 */
    volatile uint64_t total_frames;
    volatile uint64_t dropped_frames;
    volatile uint64_t overrun_count;
    volatile uint64_t missed_deadlines;
    volatile uint32_t max_jitter_ticks;
    volatile uint32_t hw_error_flags;

    pb2_pru_block_t blocks[PB2_PRU_BLOCK_CAPACITY];
} pb2_pru_shared_t;

#if defined(__cplusplus)
static_assert(sizeof(pb2_pru_block_t) == 152U, "pb2_pru_block_t size mismatch");
static_assert(offsetof(pb2_pru_shared_t, state) == 8U, "state offset mismatch");
static_assert(offsetof(pb2_pru_shared_t, cmd) == 12U, "cmd offset mismatch");
static_assert(offsetof(pb2_pru_shared_t, write_block_idx) == 52U, "write_block_idx offset mismatch");
static_assert(offsetof(pb2_pru_shared_t, blocks) == 104U, "blocks offset mismatch");
static_assert(sizeof(pb2_pru_shared_t) <= 32768U, "pb2_pru_shared_t exceeds 32 KB ICSS_SHARED");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(pb2_pru_block_t) == 152U, "pb2_pru_block_t size mismatch");
_Static_assert(offsetof(pb2_pru_shared_t, state) == 8U, "state offset mismatch");
_Static_assert(offsetof(pb2_pru_shared_t, cmd) == 12U, "cmd offset mismatch");
_Static_assert(offsetof(pb2_pru_shared_t, write_block_idx) == 52U, "write_block_idx offset mismatch");
_Static_assert(offsetof(pb2_pru_shared_t, blocks) == 104U, "blocks offset mismatch");
static_assert(sizeof(pb2_pru_shared_t) <= 32768U, "pb2_pru_shared_t exceeds 32 KB ICSS_SHARED");
#else
typedef char pb2_assert_block_size[(sizeof(pb2_pru_block_t) == 152U) ? 1 : -1];
typedef char pb2_assert_state_off[(offsetof(pb2_pru_shared_t, state) == 8U) ? 1 : -1];
typedef char pb2_assert_cmd_off[(offsetof(pb2_pru_shared_t, cmd) == 12U) ? 1 : -1];
typedef char pb2_assert_wblk_off[(offsetof(pb2_pru_shared_t, write_block_idx) == 52U) ? 1 : -1];
typedef char pb2_assert_tframes_off[(offsetof(pb2_pru_shared_t, total_frames) == 64U) ? 1 : -1];
typedef char pb2_assert_blocks_off[(offsetof(pb2_pru_shared_t, blocks) == 104U) ? 1 : -1];
typedef char pb2_assert_shm_size[(sizeof(pb2_pru_shared_t) <= 32768U) ? 1 : -1];
#endif

#ifdef __cplusplus
}
#endif
