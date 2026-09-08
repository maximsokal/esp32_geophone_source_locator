"""Static contracts for the ESP32 memory/runtime observability policy.

These checks protect configuration and integration intent only.  They do not
claim to emulate FreeRTOS scheduling, PSRAM allocation, SPI DMA, mbedTLS, or
the physical board.  Those properties require the target build and HIL logs.
"""

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative: str) -> str:
    return (REPO_ROOT / relative).read_text(encoding="utf-8")


def test_sdkconfig_defaults_keep_external_tls_and_enable_runtime_stats():
    defaults = read("sdkconfig.defaults")

    assert "CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=2048" in defaults
    assert "CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536" in defaults
    assert "CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y" in defaults
    assert "CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y" in defaults
    assert "# CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC is not set" in defaults
    assert "CONFIG_MBEDTLS_DYNAMIC_BUFFER=y" in defaults
    assert "CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384" in defaults
    assert "CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096" in defaults
    assert "CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y" in defaults
    assert "CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U64=y" in defaults
    assert "CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER=y" in defaults
    assert "CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID=y" in defaults


def test_runtime_observability_uses_raw_task_status_without_formatted_stats():
    source = read("components/runtime_observability/runtime_observability.c")

    assert "uxTaskGetSystemState(" in source
    assert "ulTaskGetIdleRunTimeCounterForCore(" in source
    assert "RUNTIME_CPU stage=" in source
    assert "RUNTIME_TASK name=" in source
    assert "MEM_RUNTIME stage=" in source
    assert "RUNTIME_OBSERVABILITY_PERIOD_MS 10000U" in source
    assert "vTaskGetRunTimeStats(" not in source
    assert "TaskStatus_t *s_previous_status" in source
    assert "runtime_observability_request_snapshot" in source


def test_service_stacks_use_psram_caps_without_changing_affinity():
    expected = {
        "components/storage_service/storage_service.c": "APP_TASK_STACK_STORAGE_SERVICE",
        "components/object_service/object_service.c": "APP_TASK_STACK_OBJECT_SERVICE",
        "components/measurement_service/measurement_service.c": "APP_TASK_STACK_MEASUREMENT_SERVICE",
    }
    for relative, stack_macro in expected.items():
        source = read(relative)
        assert "xTaskCreatePinnedToCoreWithCaps(" in source
        assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in source
        assert "xTaskCreatePinnedToCore(" not in source
        assert "APP_TASK_CORE_SERVICES" in source
        assert stack_macro in source

    assert "vTaskDeleteWithCaps(" in read("components/storage_service/storage_service.c")
    assert "vTaskDeleteWithCaps(" in read("components/object_service/object_service_worker.c")
    assert "vTaskDeleteWithCaps(" in read("components/measurement_service/measurement_service.c")

    config = read("components/app_config/include/app_task_config.h")
    assert "APP_TASK_STACK_STORAGE_SERVICE 8192" in config
    assert "APP_TASK_STACK_OBJECT_SERVICE  8192" in config
    assert "APP_TASK_STACK_MEASUREMENT_SERVICE  8192" in config


def test_realtime_and_recorder_stacks_remain_on_original_path():
    config = read("components/app_config/include/app_task_config.h")
    assert "APP_TASK_STACK_AD7606          4096" in config
    assert "APP_TASK_STACK_SEISMIC_RECORDER  4096" in config
    assert "APP_TASK_CORE_REALTIME      1" in config

    recorder = read("components/seismic_recorder/seismic_recorder.c")
    acquisition = read("components/ad7606_acquisition/ad7606_acquisition.c")
    assert "xTaskCreatePinnedToCoreWithCaps(" not in recorder
    assert "xTaskCreatePinnedToCoreWithCaps(" not in acquisition


def test_https_and_tft_policies_preserve_payload_and_dma_safety():
    web_server = read("components/web_server/web_server.c")
    web_asset = read("components/web_server/web_webui_flash.c")
    tft = read("components/ui/ui_lvgl.c")

    assert "WEB_HTTPS_MAX_CLIENTS           2U" in web_server
    assert '"Connection",\n            "close"' not in web_asset
    assert '"Connection: close"' not in web_asset
    assert "#define WEB_FLASH_SEND_CHUNK_BYTES  2048U" in read("components/web_server/web_webui_flash.c")
    assert "DMA_RUNTIME owner=tft location=internal" in tft


def test_non_realtime_storage_census_and_session_paths_are_heap_owned():
    profile = read("components/object_store/object_store_profile.c")
    recorder_fs = read("components/seismic_recorder/seismic_recorder_fs.c")
    recorder_json = read("components/seismic_recorder/seismic_recorder_json.c")
    atomic_write = read("components/storage_manager/storage_manager_text_write.c")

    assert "object_store_summary_t summaries[" not in profile
    assert "calloc(" in profile
    assert "free(summaries);" in profile
    assert "char sessions_path[SEISMIC_RECORDER_PATH_SIZE]" not in recorder_fs
    assert "char session_path[SEISMIC_RECORDER_PATH_SIZE]" not in recorder_fs
    assert "char path[SEISMIC_RECORDER_PATH_SIZE]" not in recorder_json
    assert "storage_manager_atomic_paths_t *paths" in atomic_write
    assert "storage_manager_atomic_paths_t paths" not in atomic_write
