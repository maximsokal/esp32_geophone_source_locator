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
#define TEST_REC_SAMPLE_RATE_HZ 500

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
#define TEST_REC_EVENT_FRAME 2000
