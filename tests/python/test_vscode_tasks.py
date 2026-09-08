import json
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def test_vscode_tasks_cover_local_web_and_esp_surfaces_without_hidden_flash():
    tasks_path = REPO_ROOT / ".vscode" / "tasks.json"
    document = json.loads(tasks_path.read_text(encoding="utf-8"))
    tasks = document["tasks"]
    labels = {task["label"] for task in tasks}

    expected = {
        "Tests: Host C",
        "Tests: Python",
        "Tests: Web",
        "Tests: All (no flash)",
        "Build: ESP Unity",
        "Build: ESP production",
        "ESP: Flash production (COM4)",
        "ESP: Monitor logs (COM6)",
        "ESP: Unity tests (flash COM4 + logs COM6)",
        "ESP: Production pipeline HIL (COM4 + COM6)",
    }
    assert expected <= labels

    for task in tasks:
        for dependency in task.get("dependsOn", []):
            assert dependency in labels

    all_tests = next(task for task in tasks if task["label"] == "Tests: All (no flash)")
    assert all_tests["command"] == "powershell"
    assert any(str(arg).endswith("tools/run_suite.ps1") for arg in all_tests["args"])
    assert all_tests["args"][all_tests["args"].index("-Suite") + 1] == "All"
    assert "flash" not in json.dumps(all_tests["args"]).lower()

    web_tests = next(task for task in tasks if task["label"] == "Tests: Web")
    assert web_tests["command"] == "powershell"
    assert any(str(arg).endswith("tools/run_suite.ps1") for arg in web_tests["args"])
    assert web_tests["args"][web_tests["args"].index("-Suite") + 1] == "Web"

    production_flash = next(
        task for task in tasks if task["label"] == "ESP: Flash production (COM4)"
    )
    assert "COM4" in production_flash["args"]
    assert "ESP: Check COM4" in production_flash["dependsOn"]
    assert "ESP: Check COM6" not in production_flash["dependsOn"]

    monitor = next(task for task in tasks if task["label"] == "ESP: Monitor logs (COM6)")
    assert "COM6" in monitor["args"]
    assert monitor["isBackground"] is True
    assert monitor["dependsOn"] == ["ESP: Check COM6"]

    pipeline_hil = next(
        task for task in tasks
        if task["label"] == "ESP: Production pipeline HIL (COM4 + COM6)"
    )
    assert pipeline_hil["command"] == "powershell"
    assert any(str(arg).endswith("tools/run_pipeline_hil.ps1") for arg in pipeline_hil["args"])
    assert "COM4" in pipeline_hil["args"]
    assert "COM6" in pipeline_hil["args"]
    assert "-RequireFullPipeline" in pipeline_hil["args"]
    assert "-ConfirmTftRendering" not in pipeline_hil["args"]
    assert "-ConfirmKeypadResponsive" not in pipeline_hil["args"]
    assert "-ConfirmPowerButtonResponsive" not in pipeline_hil["args"]
    assert "${input:pipelineBaseUrl}" in pipeline_hil["args"]
    assert "${input:pipelineObjectId}" in pipeline_hil["args"]
    assert pipeline_hil["dependsOn"] == ["Build: ESP production"]
    assert "ESP: Production pipeline HIL (COM4 + COM6)" not in all_tests.get("dependsOn", [])

    inputs = {entry["id"]: entry for entry in document["inputs"]}
    assert inputs["pipelineBaseUrl"]["type"] == "promptString"
    assert inputs["pipelineObjectId"]["type"] == "promptString"
