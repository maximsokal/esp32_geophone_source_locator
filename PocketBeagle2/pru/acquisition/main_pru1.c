#include <stdint.h>
#include "pru_adc_shared.h"
#include "resource_table_empty.h"

#pragma DATA_SECTION(g_pb2_pru_ring, ".pb2_shared")
#pragma RETAIN(g_pb2_pru_ring)
volatile pb2_pru_ring_t g_pb2_pru_ring;

extern void pru1_acquire_loop(volatile pb2_pru_ring_t *ring, uint32_t max_iterations);

/*
 * Safe PRU1 entry point: initializes shared-memory ring header and waits
 * in idle until Linux explicitly sets session_epoch != 0 (no GPIO toggling
 * on firmware load).
 */
int main(void) {
    g_pb2_pru_ring.magic = PB2_PRU_RING_MAGIC;
    g_pb2_pru_ring.version = PB2_PRU_RING_VERSION;
    g_pb2_pru_ring.channel_count = PB2_PRU_CHANNEL_COUNT;
    g_pb2_pru_ring.profile = PB2_PRU_PROFILE_DUAL_DOUT;
    g_pb2_pru_ring.sample_rate_hz = 0U;
    g_pb2_pru_ring.write_idx = 0U;
    g_pb2_pru_ring.read_idx = 0U;
    g_pb2_pru_ring.dropped_frames = 0ULL;
    g_pb2_pru_ring.overrun_count = 0ULL;
    g_pb2_pru_ring.hw_error_flags = 0U;
    g_pb2_pru_ring.session_epoch = 0U;

    while (g_pb2_pru_ring.session_epoch == 0U) {
        __delay_cycles(10000);
    }

    pru1_acquire_loop(&g_pb2_pru_ring, 0U);
    return 0;
}
