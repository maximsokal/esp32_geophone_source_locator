from pathlib import Path
import re
import json


ROOT = Path(__file__).resolve().parents[2]


# fail-before(real): bootstrap must start and submit through the one shared HCI queue.
def test_shared_hci_queue_wiring_and_host_manifest():
    cmake = (ROOT / "nRF54L15/zephyr/CMakeLists.txt").read_text(encoding="utf-8")
    header = (ROOT / "nRF54L15/include/nrf_hci_workqueue.h").read_text(encoding="utf-8")
    queue = (ROOT / "nRF54L15/src/nrf_hci_workqueue.c").read_text(encoding="utf-8")
    bootstrap = (ROOT / "nRF54L15/src/nrf_bootstrap_runtime_zephyr.c").read_text(encoding="utf-8")
    cs_queue = (ROOT / "nRF54L15/src/nrf_cs_pair_runtime_zephyr_workqueue.inc").read_text(encoding="utf-8")
    cs_impl = "\n".join((ROOT / f"nRF54L15/src/nrf_cs_pair_runtime_zephyr_impl_{index}.inc").read_text(encoding="utf-8") for index in range(1, 4))
    inventory = (ROOT / "nRF54L15/src/nrf_thread_stack_inventory.c").read_text(encoding="utf-8")
    main = (ROOT / "nRF54L15/src/main.c").read_text(encoding="utf-8")
    manifest = json.loads((ROOT / "tests/host/manifest.d/max493c-assign-workqueue.json").read_text(encoding="utf-8"))

    assert "../src/nrf_hci_workqueue.c" in cmake
    assert "void nrf_hci_workqueue_start(void);" in header
    assert "int nrf_hci_workqueue_submit(struct k_work *work);" in header
    assert "int nrf_hci_workqueue_reschedule(" in header
    assert queue.count("K_THREAD_STACK_DEFINE(") == 1
    assert queue.count("static struct k_work_q s_hci_workqueue;") == 1
    assert '{"cs_hci_workqueue", 2048U}' in inventory
    assert inventory.count('{"cs_hci_workqueue", 2048U}') == 1

    bootstrap_start = bootstrap.split("int nrf_bootstrap_runtime_zephyr_start", 1)[1]
    assert bootstrap_start.index("nrf_hci_workqueue_start();") < bootstrap_start.index("nrf_upstream_target_zephyr_register(")
    assert "nrf_hci_workqueue_submit(&s_assign_work)" in bootstrap
    assert not re.search(r"k_work_submit\s*\(\s*&s_assign_work", bootstrap)
    assert "k_work_submit(&s_cs_execute_work)" in bootstrap

    assert "nrf_hci_workqueue_start();" in cs_queue
    assert "nrf_hci_workqueue_submit(work)" in cs_queue
    assert "nrf_hci_workqueue_reschedule(work, delay)" in cs_queue
    assert "start_cs_workqueue();" in cs_impl
    assert main.index("nrf_cs_pair_runtime_zephyr_start()") < main.index("nrf_bootstrap_runtime_zephyr_start(&node_state)")

    tests = {item["name"]: item for item in manifest["tests"]}
    assign = tests["max493c_assign_workqueue"]["sources"]
    isolation = tests["max493c_hci_queue_isolation"]["sources"]
    assert "nRF54L15/src/nrf_hci_workqueue.c" in assign
    assert "tests/host/nrf_bootstrap_runtime_zephyr_test.c" in assign
    assert "nRF54L15/src/nrf_hci_workqueue.c" in isolation
    assert "tests/host/nrf_cs_pair_runtime_workqueue_test.c" in isolation
