#pragma once

/*
 * TEST ONLY (ESP-EMU recording scenario S10). One physical setup shared by the seeded topology
 * (nrf_topology_service_test_seed_confirmed_setup), the synthetic seismic source and the calculation settings, so the
 * source calculation has something it can actually solve: a 3 m x 4 m rectangle of nodes (MAIN at the origin), a source
 * inside it, a slow medium (so 500 Hz resolves the delays) and arrival times derived from the distances.
 *
 * ADC channel -> sensor: the seeded remotes sit on mux channels 0, 2, 3 and the recorder maps a remote to the pod of its
 * mux channel, so channels 0, 2, 3 carry the geophones of node 1, 3, 4 and channel 1 has no sensor (noise only).
 */
#define TEST_REC_SPEED_MPS 150.0
#define TEST_REC_SPEED_UNCERTAINTY_MPS 40.0
#define TEST_REC_TIMING_UNCERTAINTY_US 2000.0
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
/* The rate of the synthetic source: 500 Hz is the board's, other rates are the recording-rate sweep (docs/emulator-rate-sweep). */
#if defined(CONFIG_APP_TEST_RECORDING_RATE_HZ)
#define TEST_REC_SAMPLE_RATE_HZ CONFIG_APP_TEST_RECORDING_RATE_HZ
#else
#define TEST_REC_SAMPLE_RATE_HZ 500
#endif
/* Frame counts below were set at 500 Hz; they keep their meaning in seconds at any rate. */
#define TEST_REC_AT_RATE(frames_at_500hz) (((frames_at_500hz) * (TEST_REC_SAMPLE_RATE_HZ)) / 500U)

/* Positions in mm: MAIN, node 1, node 3, node 4. */
#define TEST_REC_NODE_X_MM {0, 3000, 0, 3000}
#define TEST_REC_NODE_Y_MM {0, 0, 4000, 4000}
#define TEST_REC_NODE_ID {0U, 1U, 3U, 4U}

/* Sensor of each ADC channel as an index into the node arrays above, -1 when none. */
#define TEST_REC_CHANNEL_NODE {1, -1, 2, 3}

/* The impact: inside the rectangle, near node 3. */
#define TEST_REC_SOURCE_X_MM 500
#define TEST_REC_SOURCE_Y_MM 3000
#define TEST_REC_SOURCE_Z_MM 0

/* The single impact happens this many frames after the generator starts. */
#define TEST_REC_EVENT_FRAME TEST_REC_AT_RATE(2000U)

/* The synthetic noise is a few uV; the board's int16 codes are 152 uV. This brings noise to tens of codes. */
#define TEST_REC_SIGNAL_GAIN 3000.0f

/* Conversions the AD7606 misses under load (board log 2026-11-14: 3 of 5672). The synthetic source skips these source
 * indices while the physical time goes on, exactly like the board: {at generator frame, how many are missed}. The
 * first sits inside the recording before the event, the second after it. */
#define TEST_REC_HOLES { {TEST_REC_AT_RATE(1300U), 2U}, {TEST_REC_AT_RATE(3400U), 1U} }
