#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "runtime_observability.h"
#include "seismic_recorder.h"
#include "seismic_acquisition.h"
#include "seismic_recorder_internal.h"
#include "object_store.h"
#include "storage_manager.h"

static seismic_acquisition_snapshot_t fake_acquisition;
static FILE *fake_file;

typedef struct { unsigned held; } fake_mutex_t;
typedef struct { EventBits_t bits; } fake_event_group_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return calloc(1, sizeof(fake_mutex_t)); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout) { (void)timeout; return semaphore != NULL ? pdTRUE : pdFALSE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) { return semaphore != NULL ? pdTRUE : pdFALSE; }
void vSemaphoreDelete(SemaphoreHandle_t semaphore) { free(semaphore); }
EventGroupHandle_t xEventGroupCreate(void) { return calloc(1, sizeof(fake_event_group_t)); }
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits) { ((fake_event_group_t *)group)->bits |= bits; return ((fake_event_group_t *)group)->bits; }
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits) { ((fake_event_group_t *)group)->bits &= ~bits; return ((fake_event_group_t *)group)->bits; }
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits, BaseType_t clear_on_exit, BaseType_t wait_for_all, TickType_t timeout) { (void)clear_on_exit; (void)wait_for_all; (void)timeout; return ((fake_event_group_t *)group)->bits & bits; }
void vEventGroupDelete(EventGroupHandle_t group) { free(group); }
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task, const char *name, unsigned stack_size, void *argument, unsigned priority, TaskHandle_t *task_handle, int core_id) { (void)task; (void)name; (void)stack_size; (void)argument; (void)priority; (void)core_id; *task_handle = (TaskHandle_t)1; return pdPASS; }
void vTaskDelay(TickType_t ticks) { (void)ticks; }
void runtime_observability_beat(runtime_observability_task_t task, uint64_t progress) { (void)task; (void)progress; }
int64_t esp_timer_get_time(void) { return 1000000; }

bool storage_manager_is_ready(void) { return true; }
esp_err_t object_store_get_profile(const char *object_id, object_store_profile_t *profile) { (void)object_id; memset(profile, 0, sizeof(*profile)); return ESP_OK; }
esp_err_t seismic_acquisition_get_snapshot(seismic_acquisition_snapshot_t *snapshot) { *snapshot = fake_acquisition; return ESP_OK; }

esp_err_t seismic_recorder_fs_create_session(const char *object_id, uint32_t rate, uint8_t channels, char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE], FILE **file) { (void)object_id; (void)rate; (void)channels; snprintf(session_id, SEISMIC_RECORDER_SESSION_ID_SIZE, "S000001"); fake_file = tmpfile(); *file = fake_file; return fake_file != NULL ? ESP_OK : ESP_FAIL; }
esp_err_t seismic_recorder_json_write_metadata(const object_store_profile_t *profile, const seismic_recorder_snapshot_t *snapshot, const char *status) { (void)profile; (void)snapshot; (void)status; return ESP_OK; }
esp_err_t seismic_buffer_copy_latest(seismic_frame_t *frames, size_t max_frames, size_t *count, seismic_buffer_info_t *info) { (void)frames; (void)max_frames; *count = 0U; memset(info, 0, sizeof(*info)); info->total_frames = 0U; return ESP_OK; }
esp_err_t seismic_buffer_copy_since(uint64_t *cursor, seismic_frame_t *frames, size_t max_frames, size_t *count, uint64_t *dropped, seismic_buffer_info_t *info) { (void)cursor; (void)frames; (void)max_frames; (void)count; (void)dropped; (void)info; return ESP_ERR_TIMEOUT; }
esp_err_t seismic_recorder_fs_close(FILE *file) { if (file != NULL) fclose(file); return ESP_OK; }
esp_err_t seismic_recorder_fs_delete_session(const char *object_id, const char *session_id) { (void)object_id; (void)session_id; return ESP_OK; }
esp_err_t seismic_recorder_fs_write_frames(FILE *file, const seismic_frame_t *frames, size_t count, size_t *written) { (void)file; (void)frames; *written = count; return ESP_OK; }
esp_err_t seismic_recorder_fs_flush(FILE *file, bool sync_to_media) { (void)file; (void)sync_to_media; return ESP_OK; }
esp_err_t seismic_recorder_fs_sync(FILE *file) { (void)file; return ESP_OK; }
esp_err_t seismic_recorder_fs_list_sessions(const char *object_id, seismic_recorder_session_summary_t *sessions, size_t max, size_t *count) { (void)object_id; (void)sessions; (void)max; *count = 0U; return ESP_OK; }
esp_err_t seismic_recorder_json_read_summary(const char *object_id, const char *session_id, seismic_recorder_session_summary_t *summary) { (void)object_id; (void)session_id; (void)summary; return ESP_ERR_NOT_FOUND; }
bool seismic_recorder_json_metadata_exists(const char *object_id, const char *session_id) { (void)object_id; (void)session_id; return false; }
esp_err_t seismic_recorder_fs_build_session_path(const char *object_id, const char *session_id, char *path, size_t path_size) { (void)object_id; (void)session_id; (void)path; (void)path_size; return ESP_ERR_NOT_FOUND; }
esp_err_t seismic_recorder_waveform_read_file(FILE *file, uint64_t size, const seismic_recorder_session_summary_t *summary, uint64_t start, uint64_t count, size_t max_points, seismic_recorder_waveform_point_t *points, size_t capacity, seismic_recorder_waveform_info_t *info) { (void)file; (void)size; (void)summary; (void)start; (void)count; (void)max_points; (void)points; (void)capacity; (void)info; return ESP_ERR_NOT_FOUND; }

int main(void)
{
    memset(&fake_acquisition, 0, sizeof(fake_acquisition));
    fake_acquisition.running = true;
    fake_acquisition.source_healthy = false;
    fake_acquisition.source_name = "AD7606";
    fake_acquisition.sample_rate_hz = 500U;
    assert(seismic_recorder_init() == ESP_OK);
    assert(seismic_recorder_start("OBJ_0005") == ESP_OK);

    seismic_recorder_snapshot_t snapshot;
    assert(seismic_recorder_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.running);
    assert(snapshot.state == SEISMIC_RECORDER_RECORDING);
    assert(strcmp(snapshot.object_id, "OBJ_0005") == 0);
    return 0;
}
