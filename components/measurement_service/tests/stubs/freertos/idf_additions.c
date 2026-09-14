#include "freertos/idf_additions.h"


BaseType_t xTaskCreatePinnedToCoreWithCaps(
    TaskFunction_t task,
    const char *name,
    unsigned stack_size,
    void *argument,
    unsigned priority,
    TaskHandle_t *task_handle,
    int core_id,
    unsigned memory_caps
)
{
    (void)memory_caps;
    return xTaskCreatePinnedToCore(
        task,
        name,
        stack_size,
        argument,
        priority,
        task_handle,
        core_id
    );
}


void vTaskDeleteWithCaps(TaskHandle_t task)
{
    vTaskDelete(task);
}
