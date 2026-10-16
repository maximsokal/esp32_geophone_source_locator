#pragma once

void workflow_host_queue_reset(void);
void workflow_host_rtos_start_created_task(void);
void workflow_host_rtos_join_created_task(void);
void workflow_host_rtos_run_created_task_sync(void);
int workflow_host_rtos_task_thread_running(void);
unsigned workflow_host_rtos_external_delete_count(void);
unsigned workflow_host_rtos_queue_delete_count(void);
