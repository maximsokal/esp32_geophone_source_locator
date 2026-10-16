#pragma once

#include "freertos/FreeRTOS.h"

void workflow_host_queue_reset(void);
void workflow_host_rtos_reset_ticks(void);
void workflow_host_rtos_advance_ticks(TickType_t ticks);
TickType_t workflow_host_rtos_last_delay_wait(void);
TickType_t workflow_host_rtos_last_delay_period(void);
void workflow_host_rtos_delay_barrier_enable(void);
void workflow_host_rtos_delay_barrier_wait(void);
void workflow_host_rtos_delay_barrier_release(void);
void workflow_host_rtos_delay_barrier_disable(void);
void workflow_host_rtos_start_created_task(void);
void workflow_host_rtos_join_created_task(void);
void workflow_host_rtos_run_created_task_sync(void);
int workflow_host_rtos_task_thread_running(void);
unsigned workflow_host_rtos_external_delete_count(void);
unsigned workflow_host_rtos_queue_delete_count(void);
