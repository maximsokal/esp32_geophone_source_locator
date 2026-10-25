#pragma once


/*
 * Dual-core application policy.
 *
 * Core 0 is the service/UI/network/storage side.
 * Core 1 is reserved for timing-sensitive acquisition and later
 * signal-processing/localization work.
 */
#define APP_TASK_CORE_SERVICES      0
#define APP_TASK_CORE_REALTIME      1

/*
 * HTTP/HTTPS server task (esp_http_server). Core 0 is saturated by Wi-Fi, LVGL,
 * SD/storage, topology and INA work, while Core 1 only carries acquisition
 * (about 40 percent while recording). The server task is therefore pinned to
 * Core 1 below both acquisition priorities: it can never outrank AD7606 or the
 * geophone ADC there, and on Core 0 it would outrank the seismic recorder
 * (priority 3), the only consumer that protects the finite ring from overflow.
 * Revert by pointing APP_TASK_CORE_WEB_SERVER back at APP_TASK_CORE_SERVICES
 * only after an A/B measurement of AD7606 jitter and UI frame time.
 */
#define APP_TASK_CORE_WEB_SERVER    APP_TASK_CORE_REALTIME
#define APP_TASK_PRIORITY_WEB_SERVER 5
_Static_assert(
    APP_TASK_PRIORITY_WEB_SERVER < 6,
    "web server must stay below the geophone ADC (6) and AD7606 (7) tasks"
);


/*
 * Core 1 acquisition tasks.
 */
#define APP_TASK_PRIORITY_AD7606          7
#define APP_TASK_STACK_AD7606          4096

#define APP_TASK_PRIORITY_GEOPHONE_ADC    6
#define APP_TASK_STACK_GEOPHONE_ADC    4096

#define APP_TASK_PRIORITY_SEISMIC_TEST    4
#define APP_TASK_STACK_SEISMIC_TEST    3072


/*
 * Core 0 application-owned service/UI/storage tasks.
 */
#define APP_TASK_PRIORITY_APP_EVENT_LOOP  6
#define APP_TASK_STACK_APP_EVENT_LOOP  6144

/* Optional ESP Insights retry work must not run inside the shared APP_EVENT
 * callback. It stays on the service core at the lowest application priority. */
#define APP_TASK_PRIORITY_INSIGHTS        1
#define APP_TASK_STACK_INSIGHTS        4096

/*
 * nRF topology discovery is low-rate control-plane work. Keep TCA selection,
 * assignment polling and hot-plug reconciliation on the service core and at
 * priority 2, below UI and measurement work, with room for protocol/log parsing.
 */
#define APP_TASK_PRIORITY_NRF_TOPOLOGY    2
#define APP_TASK_STACK_NRF_TOPOLOGY    8192

/*
 * esp_lvgl_port owns the LVGL handler task. It stays on Core 0 so
 * rendering and the shared TFT/SD SPI2 side never migrate onto the
 * real-time acquisition core. Keep 10 KiB for synchronous screen builds and
 * their nested LVGL object construction and formatting calls.
 */
#define APP_TASK_PRIORITY_LVGL            4
#define APP_TASK_STACK_LVGL           10240

#define APP_TASK_PRIORITY_STORAGE_SERVICE 3
#define APP_TASK_STACK_STORAGE_SERVICE 8192

/*
 * Explicit SD reformat is low-priority service-core work. Filesystem calls
 * yield after each successful sector-write batch; the external stack is safe
 * while FatFs/SDSPI run with flash cache enabled and falls back to internal
 * RAM if PSRAM allocation is unavailable.
 */
#define APP_TASK_PRIORITY_SD_FORMAT       2
#define APP_TASK_STACK_SD_FORMAT       12288

#define APP_TASK_PRIORITY_OBJECT_SERVICE  3
#define APP_TASK_STACK_OBJECT_SERVICE  8192

/*
 * Full object metadata recovery is intentionally background work. Startup
 * repairs only the active object synchronously, then this low-priority worker
 * scans the remaining directories one bounded storage window at a time.
 */
#define APP_TASK_PRIORITY_OBJECT_RECOVERY 1
#define APP_TASK_STACK_OBJECT_RECOVERY  9216

/*
 * Measurement commands can stop/finalize the recorder and delete a reviewed
 * session. Keep that work off the LVGL event task while retaining normal
 * service-core scheduling priority.
 */
#define APP_TASK_PRIORITY_MEASUREMENT_SERVICE 3
#define APP_TASK_STACK_MEASUREMENT_SERVICE  8192
#define APP_TASK_STACK_MEASUREMENT_SERVICE_HWM_SAFETY_MARGIN_BYTES  1024

/*
 * Recorder drains the lock-free seismic ring in bounded batches and performs
 * SD writes on the service core. Keep it below LVGL, but at the same priority
 * as other storage services: a continuously-ready bulk HTTP/SD producer must
 * not outrank the only consumer that protects the finite seismic ring from
 * overflow. Shared SPI2 access is still serialized by storage_manager.
 */
#define APP_TASK_PRIORITY_SEISMIC_RECORDER 3
#define APP_TASK_STACK_SEISMIC_RECORDER  4096

/*
 * Slow 1 Hz power telemetry. Keep it below recorder/UI/storage work so I2C
 * housekeeping cannot interfere with real-time acquisition or SD draining.
 */
#define APP_TASK_PRIORITY_INA3221          1
#define APP_TASK_STACK_INA3221          4096

/*
 * Low-priority runtime diagnostics. Its stack is allocated explicitly from
 * PSRAM by runtime_observability; this task never runs during flash-cache-off
 * code and does not participate in the acquisition path.
 */
#define APP_TASK_PRIORITY_RUNTIME_OBSERVABILITY 1
#define APP_TASK_STACK_RUNTIME_OBSERVABILITY 4096

/*
 * Object/session index refreshes run off the HTTP task. The worker is allowed
 * to spend time in FATFS/SD, but remains below the recorder and UI services.
 */
#define APP_TASK_PRIORITY_WEB_STATUS_CACHE 1
#define APP_TASK_STACK_WEB_STATUS_CACHE 8192

/*
 * mDNS and HTTPS lifecycle work must not execute in the ESP-IDF sys_evt task.
 * TLS setup and route registration are stack-heavy, so give them a dedicated
 * service-core task with a PSRAM-backed stack.
 */
#define APP_TASK_PRIORITY_WEB_LIFECYCLE 2
#define APP_TASK_STACK_WEB_LIFECYCLE 8192

/*
 * MAX-118 whole-object emergency-stop watch. Polls nRF topology sensor cache
 * for altitude jumps/multi-axis shifts only while a recording is active and
 * otherwise idles; kept at the same low control-plane priority as nRF
 * topology discovery, which it reads from.
 */
#define APP_TASK_PRIORITY_NRF_ESTOP_MONITOR 1
#define APP_TASK_STACK_NRF_ESTOP_MONITOR 4096

/*
 * MAX-118 live per-pod barometer surfacing (Measurements screen / Web status).
 * Read-only bridge from the existing nRF topology sensor cache into
 * device_status; same low control-plane priority as the other topology-cache
 * consumers so it cannot outrank acquisition/UI work.
 */
#define APP_TASK_PRIORITY_POD_STATUS_BRIDGE 1
#define APP_TASK_STACK_POD_STATUS_BRIDGE 4096

/*
 * "Calculate source" background job (localization_job): seconds of solver
 * work per run, so it runs on the signal-processing core at the idle
 * priority -- acquisition always preempts it, and IDLE1 (which yields to
 * it) still runs every tick, feeding the task watchdog. Its 16 KiB stack
 * (LOCALIZATION_JOB_STACK_BYTES) lives in PSRAM; every run reports the
 * measured high-water mark.
 */
#define APP_TASK_PRIORITY_LOCALIZATION_JOB 0
