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
 * Core 0 application-owned service/UI tasks.
 */
#define APP_TASK_PRIORITY_UI_EVENT        6
#define APP_TASK_STACK_UI_EVENT        6144

#define APP_TASK_PRIORITY_POWER_BUTTON    5
#define APP_TASK_STACK_POWER_BUTTON    3072

#define APP_TASK_PRIORITY_KEYPAD          4
#define APP_TASK_STACK_KEYPAD          8192

#define APP_TASK_PRIORITY_STORAGE_SERVICE 3
#define APP_TASK_STACK_STORAGE_SERVICE 8192

#define APP_TASK_PRIORITY_OBJECT_SERVICE  3
#define APP_TASK_STACK_OBJECT_SERVICE  8192
