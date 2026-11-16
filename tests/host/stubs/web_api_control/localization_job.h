#pragma once

#include <stdint.h>

#include "esp_err.h"

#define LOCALIZATION_JOB_DEFAULT_BUDGET_MS 60000U
#define LOCALIZATION_JOB_MIN_BUDGET_MS 1000U
#define LOCALIZATION_JOB_MAX_BUDGET_MS 600000U

/* The runs route needs only the status JSON; the control route (which
 * includes the stub localization_calculate.h first) also submits. */
esp_err_t localization_job_status_json(char **json_out);

/* The runs route's import checks that the device is not calculating the very session it is handed a result for. */
typedef enum
{
    LOCALIZATION_JOB_IDLE = 0,
    LOCALIZATION_JOB_QUEUED,
    LOCALIZATION_JOB_RUNNING,
    LOCALIZATION_JOB_SUCCEEDED,
    LOCALIZATION_JOB_FAILED,
} localization_job_state_name_t;

typedef struct
{
    int state;
    char object_id[16];
    char session_id[16];
} localization_job_status_t;

void localization_job_get_status(localization_job_status_t *status_out);

#ifdef LOCALIZATION_CALCULATE_STUB_MODEL_NAME_SIZE
esp_err_t localization_job_submit(
    const char *object_id,
    const char *session_id,
    const localization_calculate_config_t *config,
    uint32_t budget_ms,
    uint32_t *job_id_out
);
#endif
