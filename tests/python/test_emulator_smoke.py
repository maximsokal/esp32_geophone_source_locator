"""Unit tests for ESP-EMU smoke evaluators and pure helpers."""

from __future__ import annotations

import ast
import importlib.util
import json
import shlex
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[2]
TOOL_PATH = REPO_ROOT / "tools" / "run_emulator_smoke.py"
FIXTURES = REPO_ROOT / "tests" / "python" / "fixtures" / "emulator"
SPEC = importlib.util.spec_from_file_location("run_emulator_smoke", TOOL_PATH)
assert SPEC is not None and SPEC.loader is not None
smoke = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(smoke)


def read_log(name: str) -> str:
    return (FIXTURES / name).read_text(encoding="utf-8")


def read_json(name: str) -> dict:
    return json.loads((FIXTURES / name).read_text(encoding="utf-8"))


def test_evaluate_s1_accepts_boot_markers_and_sixty_second_run():
    ok, reasons = smoke.evaluate_s1(read_log("s1_boot_ok.log"), {"duration_seconds": 60.1})
    assert ok, reasons


def test_evaluate_s1_rejects_panic_and_short_runtime():
    ok, reasons = smoke.evaluate_s1(
        read_log("s1_boot_ok.log") + read_log("panic_excerpt.log"),
        {"duration_seconds": 59.9},
    )
    assert not ok
    assert any("panic/fatal marker" in reason for reason in reasons)
    assert any("at least 60s" in reason for reason in reasons)


def test_evaluate_s2_accepts_reset_ack_and_distinct_boot_ids():
    ok, reasons = smoke.evaluate_s2(
        read_log("s2_two_boots.log"), {"control": {"reset": "ok"}}
    )
    assert ok, reasons


@pytest.mark.parametrize("reset_reply", ["", "err"])
def test_evaluate_s2_rejects_empty_or_invalid_reset_ack(reset_reply: str):
    ok, reasons = smoke.evaluate_s2(
        read_log("s2_two_boots.log"), {"control": {"reset": reset_reply}}
    )
    assert not ok
    assert any("acknowledge reset" in reason for reason in reasons)


def test_evaluate_s2_rejects_single_boot_or_missing_reset_ack():
    ok, reasons = smoke.evaluate_s2(
        read_log("s2_one_boot.log"), {"control_reply": "timeout"}
    )
    assert not ok
    assert any("two Hardware bring-up" in reason for reason in reasons)
    assert any("two RESET_REASON" in reason for reason in reasons)
    assert any("acknowledge reset" in reason for reason in reasons)


def test_s2_control_client_builds_timeout_read_loop():
    script = smoke.build_scenario_script(
        "S2",
        "/mnt/e/repo",
        smoke.DEFAULT_EMU,
        "/mnt/e/image.bin",
        "/mnt/e/app.elf",
        "/mnt/e/out/s2.log",
        "/mnt/e/out",
    )
    inline_python = script.split("python3 - <<'PY'\n", 1)[1].split("\nPY\n", 1)[0]
    tree = ast.parse(inline_python)

    calls = [node for node in ast.walk(tree) if isinstance(node, ast.Call)]
    assert any(
        isinstance(call.func, ast.Attribute)
        and call.func.attr == "settimeout"
        and call.args
        and isinstance(call.args[0], ast.Constant)
        and call.args[0].value == 5
        for call in calls
    )
    read_loops = [node for node in ast.walk(tree) if isinstance(node, ast.While)]
    assert any(
        any(
            isinstance(child, ast.Call)
            and isinstance(child.func, ast.Attribute)
            and child.func.attr == "recv"
            for child in ast.walk(loop)
        )
        for loop in read_loops
    )
    assert any(
        isinstance(handler, ast.ExceptHandler)
        and isinstance(handler.type, ast.Attribute)
        and handler.type.attr == "timeout"
        for handler in ast.walk(tree)
    )


def test_evaluate_s3_accepts_four_megabyte_psram_boot():
    ok, reasons = smoke.evaluate_s3(read_log("s3_psram_4m.log"))
    assert ok, reasons


def test_evaluate_s3_rejects_missing_psram_marker():
    ok, reasons = smoke.evaluate_s3(read_log("s3_missing_psram.log"))
    assert not ok
    assert any("4096 KB" in reason for reason in reasons)


def test_evaluate_s4_accepts_setup_redirects_and_http_status_404():
    ok, reasons = smoke.evaluate_s4(
        read_log("s4_http_boot.log"), {"http": read_json("http_redirects_ok.json")}
    )
    assert ok, reasons


def test_evaluate_s4_rejects_http_200_instead_of_redirect():
    ok, reasons = smoke.evaluate_s4(
        read_log("s4_http_boot.log"), {"http": read_json("http_redirects_200.json")}
    )
    assert not ok
    assert any("GET /setup returned 200" in reason for reason in reasons)


def test_merge_flash_image_uses_offsets_and_ff_padding():
    image = smoke.merge_flash_image(b"BOOT", b"PART", b"APP")

    assert len(image) == 16 * 1024 * 1024
    assert image[:4] == b"BOOT"
    assert image[0x8000:0x8004] == b"PART"
    assert image[0x10000:0x10003] == b"APP"
    assert image[4:0x8000] == b"\xff" * (0x8000 - 4)
    assert image[0x8004:0x10000] == b"\xff" * (0x10000 - 0x8004)
    assert image[-1:] == b"\xff"


def test_merge_flash_image_rejects_segment_overflow():
    with pytest.raises(ValueError, match="exceeds"):
        smoke.merge_flash_image(b"", b"", b"X" * 0x21, flash_size=0x10020)


def test_wsl_path_conversion_keeps_space_inside_mounted_path():
    assert (
        smoke.to_wsl_path(
            r"E:\Beetroot Academy\Embedded\esp32_geophone_source_locator\out dir\boot.log"
        )
        == "/mnt/e/Beetroot Academy/Embedded/esp32_geophone_source_locator/out dir/boot.log"
    )


def test_wsl_path_conversion_handles_other_drive_letters_and_spaces():
    assert (
        smoke.to_wsl_path(r"C:\Program Files\ESP Tools\runner.sh")
        == "/mnt/c/Program Files/ESP Tools/runner.sh"
    )


def test_shell_command_quotes_paths_and_round_trips_argv():
    argv = [
        smoke.DEFAULT_EMU,
        "--firmware",
        "/mnt/e/Beetroot Academy/Embedded/merged image.bin",
        "--elf",
        "/mnt/e/Beetroot Academy/Embedded/app.elf",
    ]
    quoted = smoke.shell_join(argv)
    assert shlex.split(quoted) == argv
    assert "'" in quoted

    script = smoke.build_scenario_script(
        "S1",
        "/mnt/e/Beetroot Academy/Embedded/repo",
        smoke.DEFAULT_EMU,
        "/mnt/e/Beetroot Academy/Embedded/merged image.bin",
        "/mnt/e/Beetroot Academy/Embedded/app.elf",
        "/mnt/e/Beetroot Academy/Embedded/out dir/s1.log",
        "/mnt/e/Beetroot Academy/Embedded/out dir",
    )
    assert "cd -- '/mnt/e/Beetroot Academy/Embedded/repo'" in script
    assert "'/mnt/e/Beetroot Academy/Embedded/merged image.bin'" in script
    assert "set -u\n" in script
    assert "\r\n" not in script
    assert "pkill" not in script


def test_wsl_argv_passes_script_path_as_one_argument():
    script_path = "/mnt/e/Beetroot Academy/Embedded/emu smoke/s1.sh"
    assert smoke.build_wsl_argv(script_path) == [
        "wsl.exe",
        "-d",
        "Ubuntu",
        "-u",
        "root",
        "--",
        "bash",
        script_path,
    ]


def test_all_scenario_scripts_use_lf_line_endings():
    paths = (
        "/mnt/e/repo",
        smoke.DEFAULT_EMU,
        "/mnt/e/out/merged image.bin",
        "/mnt/e/out/app.elf",
        "/mnt/e/out/smoke.log",
        "/mnt/e/out",
    )
    for scenario in smoke.SCENARIOS:
        script = smoke.build_scenario_script(scenario, *paths)
        assert "\r\n" not in script
        assert script.endswith("\n")
        if scenario in ("S2", "S4"):
            assert " &\nEP=$!\n" in script


def test_scenario_argv_includes_measured_flags():
    argv, timeout, outer_timeout = smoke.build_emulator_argv(
        "S4", smoke.DEFAULT_EMU, "/mnt/e/image.bin", "/mnt/e/app.elf"
    )
    assert timeout == 100
    assert outer_timeout > timeout
    assert argv[argv.index("--wifi-fw-ap-password") + 1] == "GEOLOCATOR"
    assert argv[argv.index("--net") + 1] == "user,hostfwd=tcp::18080-:80"
    assert smoke.build_wsl_argv("true")[:6] == [
        "wsl.exe",
        "-d",
        "Ubuntu",
        "-u",
        "root",
        "--",
    ]
