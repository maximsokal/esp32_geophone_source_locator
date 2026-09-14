from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def read(relative: str) -> str:
    return (REPO_ROOT / relative).read_text(encoding="utf-8")


def function_body(source: str, signature: str, next_signature: str) -> str:
    start = source.index(signature)
    end = source.index(next_signature, start)
    return source[start:end]


def test_project_keeps_idf_task_watchdog_enabled_at_startup():
    sdkconfig = read("sdkconfig.defaults")

    assert "CONFIG_ESP_TASK_WDT_EN=y" in sdkconfig
    assert "CONFIG_ESP_TASK_WDT_INIT=y" in sdkconfig
    assert "CONFIG_ESP_TASK_WDT_TIMEOUT_S=5" in sdkconfig
    assert "CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y" in sdkconfig
    assert "CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=y" in sdkconfig


def test_storage_worker_is_watched_only_while_a_command_is_active():
    source = read("components/storage_service/storage_service.c")
    task = function_body(
        source,
        "static void storage_service_task(",
        "static esp_err_t storage_service_submit(",
    )

    receive = task.index("xQueueReceive(")
    stop_check = task.index("STORAGE_COMMAND_STOP")
    begin = task.index("storage_service_watchdog_begin()")
    process = task.index("storage_service_process_info(")
    end = task.index("storage_service_watchdog_end(")
    notify = task.index("storage_service_notify()")

    assert receive < stop_check < begin < process < end < notify
    assert "esp_task_wdt_add(NULL)" in source
    assert "esp_task_wdt_delete(NULL)" in source
    assert "esp_task_wdt_init(" not in source
    assert "esp_task_wdt_reconfigure(" not in source


def test_recursive_erase_feeds_only_after_completed_filesystem_mutations():
    source = read("components/storage_manager/storage_manager_tree.c")
    cooperate = function_body(
        source,
        "static void storage_manager_tree_cooperate(",
        "static esp_err_t storage_manager_remove_tree_locked(",
    )

    increment = cooperate.index("(*operation_count)++")
    checkpoint = cooperate.index("storage_manager_tree_watchdog_checkpoint()")
    delay = cooperate.index("vTaskDelay(")

    assert increment < checkpoint < delay
    assert "STORAGE_TREE_COOPERATE_INTERVAL" in cooperate
    assert "esp_task_wdt_status(NULL) == ESP_OK" in source
    assert "esp_task_wdt_reset()" in source


def test_background_recovery_watches_only_the_recovery_run_not_callback_teardown():
    worker = read("components/object_recovery/object_recovery_worker.c")
    task = function_body(
        worker,
        "static void object_recovery_background_task(",
        "esp_err_t object_recovery_start_background_with_callback(",
    )

    begin = task.index("object_recovery_watchdog_begin()")
    run = task.index("object_recovery_run_cooperative(")
    end = task.index("object_recovery_watchdog_end(")
    callback = task.index("complete_fn(err, complete_context)")

    assert begin < run < end < callback
    assert "esp_task_wdt_add(NULL)" in worker
    assert "esp_task_wdt_delete(NULL)" in worker
    assert "esp_task_wdt_init(" not in worker
    assert "esp_task_wdt_reconfigure(" not in worker


def test_recovery_feeds_after_directory_and_object_progress_from_the_owner_task():
    source = read("components/object_recovery/object_recovery.c")

    assert "esp_task_wdt_status(NULL) == ESP_OK" in source
    assert "esp_task_wdt_reset()" in source

    collect = function_body(
        source,
        "static esp_err_t object_recovery_collect_ids(",
        "static bool object_recovery_should_stop(",
    )
    assert collect.index("readdir(directory)") < collect.index(
        "object_recovery_watchdog_checkpoint()"
    )

    run = function_body(
        source,
        "esp_err_t object_recovery_run_cooperative(",
        "esp_err_t object_recovery_run(void)",
    )
    completed = run.index("++processed")
    checkpoint = run.index("object_recovery_watchdog_checkpoint()")
    assert completed < checkpoint


def test_components_declare_esp_system_for_task_watchdog_header():
    for relative in (
        "components/storage_service/CMakeLists.txt",
        "components/storage_manager/CMakeLists.txt",
        "components/object_recovery/CMakeLists.txt",
    ):
        cmake = read(relative)
        assert "esp_system" in cmake, relative
