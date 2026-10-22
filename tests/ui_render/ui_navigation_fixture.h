#pragma once
#include "ui_internal.h"
#include "workflow_service.h"
#include "hw_inventory.h"

extern ui_context_t render_context;
extern workflow_snapshot_t render_snapshot;
extern measurement_service_snapshot_t render_measurement;
extern hw_module_info_t render_modules[HW_MODULE_COUNT];
extern uint64_t render_now_ms;
extern bool render_active_object, render_storage_ready, render_wifi_saved, render_service_busy;
extern esp_err_t render_service_result, render_acquisition_result;
extern unsigned render_acquisition_starts, render_list_calls, render_active_calls;
extern unsigned render_delete_calls, render_erase_calls, render_info_calls;
extern unsigned render_wifi_setup_calls, render_wifi_forget_calls;
extern unsigned render_command_count;
extern workflow_command_t render_last_command;
extern unsigned render_heap_allocations;
extern size_t render_heap_bytes;

void render_navigation_start(const char *directory);
void render_navigation_capture(const char *name);
void render_navigation_finish(void);
bool render_workflow_exists(void);
bool render_workflow_result_pending(void);
bool render_workflow_result_ready(void);
uint32_t render_workflow_result_generation(void);
size_t render_workflow_action_count(void);
bool render_preview_open(void);
void render_preview_complete(bool stale);
void render_run_task(const char *name, void (*before_completion)(void));
