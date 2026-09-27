#pragma once

#include "localization_job.h"

extern unsigned workflow_test_scan_calls;
extern bool workflow_test_scan_present;
extern esp_err_t workflow_test_settings_error;
extern const char *workflow_test_settings_json;
extern localization_job_status_t workflow_test_job_status;
extern esp_err_t workflow_test_job_submit_result;
extern unsigned workflow_test_job_submit_calls;
extern uint32_t workflow_test_next_job_id;
extern localization_calculate_config_t workflow_test_submitted_config;
extern uint32_t workflow_test_submitted_budget_ms;
extern char workflow_test_submitted_object_id[16];
extern char workflow_test_submitted_session_id[16];

void workflow_service_host_fakes_reset(void);
