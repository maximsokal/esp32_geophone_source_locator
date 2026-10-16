#pragma once

/*
 * Provisional stop bounds until target worst-case I/O measurements are
 * available. Tune from measured worst-case latency plus margin before release.
 */
#ifndef APP_STOP_TIMEOUT_WORKER_MS
#define APP_STOP_TIMEOUT_WORKER_MS 5000U
#endif

#ifndef APP_STOP_TIMEOUT_RECORDING_MS
#define APP_STOP_TIMEOUT_RECORDING_MS 60000U
#endif

#ifndef APP_STOP_TIMEOUT_FORMAT_MS
#define APP_STOP_TIMEOUT_FORMAT_MS 60000U
#endif

#if APP_STOP_TIMEOUT_WORKER_MS == 0U || \
    APP_STOP_TIMEOUT_RECORDING_MS == 0U || \
    APP_STOP_TIMEOUT_FORMAT_MS == 0U
#error "Stop timeout limits must be positive"
#endif
