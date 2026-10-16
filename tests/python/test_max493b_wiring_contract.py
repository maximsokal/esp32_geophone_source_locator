from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
CS_IMPL = [
    ROOT / "nRF54L15/src/nrf_cs_pair_runtime_zephyr_impl_1.inc",
    ROOT / "nRF54L15/src/nrf_cs_pair_runtime_zephyr_impl_2.inc",
    ROOT / "nRF54L15/src/nrf_cs_pair_runtime_zephyr_impl_3.inc",
]


# fail-before(real): the runtime start path must ready the queue before Bluetooth can post events.
def test_runtime_start_queues_before_bluetooth_enable():
    source = CS_IMPL[2].read_text(encoding="utf-8")
    start = source.split("int nrf_cs_pair_runtime_zephyr_start(void)", 1)[1].split(
        "int nrf_cs_pair_runtime_zephyr_set_node_id", 1
    )[0]
    assert start.index("start_cs_workqueue();") < start.index("bt_enable(NULL)")
    assert "if (err != 0) {\n        return err;\n    }" in start
    queue = (ROOT / "nRF54L15/src/nrf_hci_workqueue.c").read_text(
        encoding="utf-8"
    )
    assert "if (s_hci_workqueue_started)" in queue
    assert "k_work_queue_init(&s_hci_workqueue);" in queue
    assert "k_work_queue_start(" in queue


# fail-before(real): event, cleanup, execute, and timeout items must never fall back to the system queue.
def test_cs_items_use_only_the_dedicated_queue():
    sources = "\n".join(path.read_text(encoding="utf-8") for path in CS_IMPL)
    assert "submit_cs_work(&s_event_work)" in sources
    assert sources.count("submit_cs_work(&s_cleanup_work)") == 2
    assert "submit_cs_work(&s_execute_work)" in sources
    assert "reschedule_cs_work(&s_timeout_work" in sources
    assert not re.search(r"\bk_work_(?:submit|schedule|reschedule)\s*\(", sources)


# characterization: preserve the watchdog progress participant and system-queue health probes.
def test_watchdog_and_twis_work_remain_on_system_queue():
    watchdog = (ROOT / "nRF54L15/src/nrf_watchdog_zephyr.c").read_text(encoding="utf-8")
    bootstrap = (ROOT / "nRF54L15/src/nrf_bootstrap_runtime_zephyr.c").read_text(
        encoding="utf-8"
    )
    runtime = CS_IMPL[2].read_text(encoding="utf-8")
    assert "k_work_submit(&s_workqueue_probe)" in watchdog
    assert "k_work_submit(&s_cs_execute_work)" in bootstrap
    assert "nrf_hci_workqueue_submit(&s_assign_work)" in bootstrap
    assert "NRF_WATCHDOG_PARTICIPANT_BT_CS_PROGRESS" in runtime
    assert "atomic_get(&s_execute_pending) != 0 || nrf_cs_pair_adapter_is_active(&s_adapter)" in runtime


# characterization: keep the dedicated HCI worker within the existing inventory and sensor priority.
def test_queue_stack_priority_and_inventory_match():
    queue = (ROOT / "nRF54L15/src/nrf_hci_workqueue.c").read_text(
        encoding="utf-8"
    )
    inventory = (ROOT / "nRF54L15/src/nrf_thread_stack_inventory.c").read_text(
        encoding="utf-8"
    )
    sensor = (ROOT / "nRF54L15/src/nrf_sensor_upstream_runtime_zephyr.c").read_text(
        encoding="utf-8"
    )
    assert "#define NRF_HCI_WORKQUEUE_STACK_SIZE 2048U" in queue
    assert "#define NRF_HCI_WORKQUEUE_PRIORITY K_PRIO_PREEMPT(10)" in queue
    assert "#define NRF_SENSOR_WORKER_PRIORITY K_PRIO_PREEMPT(10)" in sensor
    assert '{"cs_hci_workqueue", 2048U}' in inventory
