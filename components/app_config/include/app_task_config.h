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
 * esp_lvgl_port owns the LVGL handler task. It stays on Core 0 so
 * rendering and the shared TFT/SD SPI2 side never migrate onto the
 * real-time acquisition core.
 */
#define APP_TASK_PRIORITY_LVGL            4
#define APP_TASK_STACK_LVGL            7168

#define APP_TASK_PRIORITY_STORAGE_SERVICE 3
#define APP_TASK_STACK_STORAGE_SERVICE 8192

#define APP_TASK_PRIORITY_OBJECT_SERVICE  3
#define APP_TASK_STACK_OBJECT_SERVICE  8192

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
