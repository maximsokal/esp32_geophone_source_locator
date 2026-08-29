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
 * AD7606 acquisition owns the dedicated SPI3 bus on Core 1.
 */
#define APP_TASK_PRIORITY_AD7606    7
#define APP_TASK_STACK_AD7606       4096
