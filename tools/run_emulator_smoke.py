#!/usr/bin/env python3
"""Run short ESP-EMU smoke scenarios against the emulator-profile firmware."""

from __future__ import annotations

import argparse
import json
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path, PureWindowsPath
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_EMU = "/opt/esp-emu-dl/esp-emu-0.49.0-x86_64-unknown-linux-gnu/esp-emu"
DEFAULT_IMAGE = ".test-build/emu/merged_emu.bin"
DEFAULT_ELF = ".test-build/idf-emulator/esp32_geophone_source_locator.elf"
DEFAULT_OUT_DIR = ".test-build/emu-smoke"
FLASH_SIZE = 16 * 1024 * 1024
FLASH_OFFSETS = {"bootloader": 0x0, "partition_table": 0x8000, "app": 0x10000}
SCENARIOS = ("S1", "S2", "S3", "S4")
LIMITATIONS = (
    "ESP-EMU cannot prove SD, AD7606 BUSY, TCA9548A, INA3221, or hardware timing. "
    "TFT/LVGL and ADC are not modelled and remain unverified."
)

_FATAL_LINE = re.compile(
    r"Guru Meditation|Backtrace\b|Firmware abort|\babort(?:ed|ing)?\b",
    re.IGNORECASE,
)
_RESET_BOOT_ID = re.compile(
    r"\bRESET_REASON\b[^\r\n]*\bboot_id=([0-9A-Fa-fx]+)", re.IGNORECASE
)


def _panic_lines(log_text: str) -> list[str]:
    return [
        line
        for line in log_text.splitlines()
        if "panic intercept" not in line.lower() and _FATAL_LINE.search(line)
    ]


def _common_reasons(log_text: str, extra: dict[str, Any] | None) -> list[str]:
    reasons = []
    panic_lines = _panic_lines(log_text)
    if panic_lines:
        reasons.append(f"panic/fatal marker: {panic_lines[0].strip()}")
    if extra:
        if extra.get("run_error"):
            reasons.append(f"runner error: {extra['run_error']}")
        return_code = extra.get("wsl_returncode")
        if return_code is not None and return_code != 0:
            reasons.append(f"WSL scenario command exited with {return_code}")
    return reasons


def evaluate_s1(log_text: str, extra: dict[str, Any] | None = None) -> tuple[bool, list[str]]:
    """Evaluate boot and 60-second stability markers."""
    reasons = _common_reasons(log_text, extra)
    for marker in (
        "Hardware bring-up completed",
        "Display: SKIPPED",
        "Keypad: SKIPPED",
        "BOOT_NO_STORAGE",
    ):
        if marker not in log_text:
            reasons.append(f"missing required log marker: {marker}")
    if extra is not None and "duration_seconds" in extra:
        try:
            duration = float(extra["duration_seconds"])
        except (TypeError, ValueError):
            reasons.append("measured run duration is invalid")
        else:
            if duration < 60:
                reasons.append(f"run lasted {duration:.1f}s; at least 60s is required")
    return not reasons, reasons


def evaluate_s2(log_text: str, extra: dict[str, Any] | None = None) -> tuple[bool, list[str]]:
    """Evaluate reset-channel acknowledgement and evidence of two boots."""
    reasons = _common_reasons(log_text, extra)
    if log_text.count("Hardware bring-up completed") < 2:
        reasons.append("expected two Hardware bring-up completed markers")
    boot_ids = _RESET_BOOT_ID.findall(log_text)
    if len(boot_ids) < 2:
        reasons.append("expected two RESET_REASON lines containing boot_id")
    elif len(set(boot_ids)) < 2:
        reasons.append("RESET_REASON boot_id values did not change across reset")
    control = (extra or {}).get("control", {})
    reset_reply = (control.get("reset") if isinstance(control, dict) else None) or (
        (extra or {}).get("control_reply")
    )
    if str(reset_reply).strip().lower() != "ok":
        reasons.append("control channel did not acknowledge reset with 'ok'")
    return not reasons, reasons


def evaluate_s3(log_text: str, extra: dict[str, Any] | None = None) -> tuple[bool, list[str]]:
    """Evaluate the 4 MB PSRAM startup case."""
    reasons = _common_reasons(log_text, extra)
    if "PSRAM configured as 4096 KB" not in log_text:
        reasons.append("missing PSRAM configured as 4096 KB marker")
    if "Hardware bring-up completed" not in log_text:
        reasons.append("missing Hardware bring-up completed marker")
    return not reasons, reasons


def evaluate_s4(log_text: str, extra: dict[str, Any] | None = None) -> tuple[bool, list[str]]:
    """Evaluate HTTP bootstrap redirects and the intentionally absent HTTP status API."""
    reasons = _common_reasons(log_text, extra)
    if "HTTP bootstrap server started" not in log_text:
        reasons.append("missing HTTP bootstrap server started marker")
    responses = (extra or {}).get("http")
    if not isinstance(responses, dict):
        reasons.append("HTTP probe results are missing")
        return not reasons, reasons

    for path in ("/setup", "/"):
        response = responses.get(path)
        status = response.get("status") if isinstance(response, dict) else None
        if status != 302:
            reasons.append(f"GET {path} returned {status!r}; expected HTTP 302")
        body = response.get("body", "") if isinstance(response, dict) else ""
        if "Install and confirm the GeoLocator Root CA" not in str(body):
            reasons.append(f"GET {path} body is missing the root CA confirmation text")

    status_response = responses.get("/api/status")
    status_code = status_response.get("status") if isinstance(status_response, dict) else None
    if status_code != 404:
        reasons.append(f"GET /api/status returned {status_code!r}; expected HTTP 404")
    return not reasons, reasons


EVALUATORS = {
    "S1": evaluate_s1,
    "S2": evaluate_s2,
    "S3": evaluate_s3,
    "S4": evaluate_s4,
}


def merge_flash_image(
    bootloader: bytes,
    partition_table: bytes,
    app: bytes,
    flash_size: int = FLASH_SIZE,
) -> bytes:
    """Create a raw 16 MB flash image with ESP-IDF's standard emulator offsets."""
    if flash_size <= 0:
        raise ValueError("flash_size must be positive")
    image = bytearray(b"\xff" * flash_size)
    occupied: list[tuple[int, int, str]] = []
    for name, payload in (
        ("bootloader", bootloader),
        ("partition_table", partition_table),
        ("app", app),
    ):
        offset = FLASH_OFFSETS[name]
        end = offset + len(payload)
        if end > flash_size:
            raise ValueError(
                f"{name} at 0x{offset:x} exceeds the {flash_size}-byte flash image"
            )
        for previous_start, previous_end, previous_name in occupied:
            if offset < previous_end and previous_start < end:
                raise ValueError(f"{name} overlaps {previous_name}")
        if payload:
            image[offset:end] = payload
            occupied.append((offset, end, name))
    return bytes(image)


def build_merged_image(build_dir: Path, image_path: Path) -> None:
    """Merge the three files and offsets declared by ESP-IDF's flasher_args.json."""
    build_dir = build_dir.resolve()
    flasher_args_path = build_dir / "flasher_args.json"
    flasher = json.loads(flasher_args_path.read_text(encoding="utf-8"))
    flash_files = flasher.get("flash_files", {})
    by_offset = {int(offset, 0): filename for offset, filename in flash_files.items()}
    expected = {
        FLASH_OFFSETS["bootloader"],
        FLASH_OFFSETS["partition_table"],
        FLASH_OFFSETS["app"],
    }
    if not expected.issubset(by_offset):
        raise ValueError("flasher_args.json is missing bootloader, partition table, or app")
    declared_size = flasher.get("flash_settings", {}).get("flash_size", "16MB")
    if str(declared_size).upper() != "16MB":
        raise ValueError(f"expected a 16MB firmware image, found {declared_size!r}")

    def read_component(offset: int) -> bytes:
        component_path = (build_dir / by_offset[offset]).resolve()
        try:
            component_path.relative_to(build_dir)
        except ValueError as exc:
            raise ValueError(f"flash component escapes build directory: {component_path}") from exc
        return component_path.read_bytes()

    image_bytes = merge_flash_image(
        read_component(FLASH_OFFSETS["bootloader"]),
        read_component(FLASH_OFFSETS["partition_table"]),
        read_component(FLASH_OFFSETS["app"]),
    )
    image_path.parent.mkdir(parents=True, exist_ok=True)
    image_path.write_bytes(image_bytes)


def to_wsl_path(path: str | Path) -> str:
    """Convert an absolute Windows drive path to its mounted WSL path."""
    windows_path = PureWindowsPath(str(path))
    drive = windows_path.drive
    if not windows_path.is_absolute() or len(drive) != 2 or not drive.endswith(":"):
        raise ValueError(f"expected an absolute drive path, got {path!r}")
    parts = windows_path.parts[1:]
    suffix = "/".join(parts)
    return f"/mnt/{drive[0].lower()}" + (f"/{suffix}" if suffix else "")


def shell_join(arguments: list[str]) -> str:
    """Quote an argv list for a single Bash command string."""
    return shlex.join([str(argument) for argument in arguments])


def build_emulator_argv(
    scenario: str,
    emu: str,
    image: str,
    elf: str,
) -> tuple[list[str], int, int]:
    """Return emulator argv, scenario timeout, and outer timeout in seconds."""
    timeout = {"S1": 70, "S2": 70, "S3": 40, "S4": 100}[scenario]
    outer_timeout = timeout + (10 if scenario != "S4" else 15)
    argv = [
        emu,
        "--chip",
        "esp32s3",
        "--firmware",
        image,
        "--elf",
        elf,
        "--timeout",
        f"{timeout}s",
    ]
    if scenario == "S2":
        argv.extend(("--control-tcp", "127.0.0.1:5556"))
    elif scenario == "S3":
        argv.extend(("--psram-size", "4M"))
    elif scenario == "S4":
        argv.extend(
            (
                "--wifi-fw-ap-password",
                "GEOLOCATOR",
                "--net",
                "user,hostfwd=tcp::18080-:80",
            )
        )
    return argv, timeout, outer_timeout


def build_wsl_argv(script_path: str, distro: str = "Ubuntu") -> list[str]:
    """Build the Windows argv used to run a script file inside WSL."""
    return ["wsl.exe", "-d", distro, "-u", "root", "--", "bash", script_path]


def build_scenario_script(
    scenario: str,
    repo_wsl: str,
    emu: str,
    image_wsl: str,
    elf_wsl: str,
    log_wsl: str,
    out_wsl: str,
) -> str:
    argv, _, outer_timeout = build_emulator_argv(
        scenario, emu, image_wsl, elf_wsl
    )
    duration_wsl = f"{out_wsl.rstrip('/')}/{scenario.lower()}.duration"
    command = (
        f"timeout --signal=TERM --kill-after=5s {outer_timeout}s "
        f"{shell_join(argv)} > {shlex.quote(log_wsl)} 2>&1"
    )
    duration_write = (
        "end_s=$(date +%s)\n"
        + f"printf 'EMU_DURATION_SECONDS=%s\\n' \"$((end_s - start_s))\" > {shlex.quote(duration_wsl)}\n"
    )
    common = (
        "#!/usr/bin/env bash\nset -u\n"
        + f"cd -- {shlex.quote(repo_wsl)}\nmkdir -p -- {shlex.quote(out_wsl)}\n"
    )
    if scenario == "S2":
        return (
            common
            + "start_s=$(date +%s)\n"
            + f"{command} &\nEP=$!\n"
            + "cleanup() { if kill -0 \"$EP\" 2>/dev/null; then "
            + "kill -TERM \"$EP\" 2>/dev/null || true; fi; }\n"
            + "trap cleanup EXIT INT TERM\n"
            + "sleep 30\n"
            + f"EMU_LOG={shlex.quote(log_wsl)} python3 - <<'PY'\n"
            + "import json, os, re, socket\n"
            + "def read_reply(sock):\n"
            + "    data = bytearray()\n"
            + "    while b'\\n' not in data and b'ok' not in data.lower():\n"
            + "        try:\n"
            + "            chunk = sock.recv(200)\n"
            + "        except socket.timeout:\n"
            + "            break\n"
            + "        if not chunk:\n"
            + "            break\n"
            + "        data.extend(chunk)\n"
            + "    return data.decode('utf-8', 'replace').strip()\n"
            + "def boot_ids():\n"
            + "    try:\n"
            + "        with open(os.environ['EMU_LOG'], encoding='utf-8', errors='replace') as log_file:\n"
            + "            return re.findall(r'\\bRESET_REASON\\b[^\\r\\n]*\\bboot_id=([0-9A-Fa-fx]+)', log_file.read(), re.IGNORECASE)\n"
            + "    except OSError:\n"
            + "        return []\n"
            + "s = socket.create_connection(('127.0.0.1', 5556), timeout=5)\n"
            + "s.settimeout(5)\n"
            + "s.sendall(b'ping\\n')\n"
            + "ping = read_reply(s)\n"
            + "initial_boot_ids = set(boot_ids())\n"
            + "s.sendall(b'reset\\n')\n"
            + "reset = read_reply(s)\n"
            + "s.close()\n"
            + "current_boot_ids = boot_ids()\n"
            + "second_boot_observed = (len(current_boot_ids) >= 2 and len(set(current_boot_ids)) >= 2 and bool(set(current_boot_ids) - initial_boot_ids))\n"
            + "if reset == '' and not second_boot_observed:\n"
            + "    retry = socket.create_connection(('127.0.0.1', 5556), timeout=5)\n"
            + "    retry.settimeout(5)\n"
            + "    retry.sendall(b'reset\\n')\n"
            + "    reset = read_reply(retry)\n"
            + "    retry.close()\n"
            + "print('CONTROL_RESULTS=' + json.dumps({'ping': ping, 'reset': reset}, separators=(',', ':')))\n"
            + "PY\ncontrol_rc=$?\n"
            + "wait \"$EP\"\nemu_rc=$?\ntrap - EXIT INT TERM\n"
            + duration_write
            + "if [ \"$control_rc\" -ne 0 ]; then exit \"$control_rc\"; fi\n"
            + "exit \"$emu_rc\"\n"
        )
    if scenario == "S4":
        return (
            common
            + "start_s=$(date +%s)\n"
            + f"{command} &\nEP=$!\n"
            + "cleanup() { if kill -0 \"$EP\" 2>/dev/null; then "
            + "kill -TERM \"$EP\" 2>/dev/null || true; fi; }\n"
            + "trap cleanup EXIT INT TERM\n"
            + "sleep 60\n"
            + "python3 - <<'PY'\n"
            + "import json, urllib.error, urllib.request\n"
            + "class NoRedirect(urllib.request.HTTPRedirectHandler):\n"
            + "    def redirect_request(self, req, fp, code, msg, headers, newurl):\n"
            + "        return None\n"
            + "opener = urllib.request.build_opener(NoRedirect)\n"
            + "def get(path):\n"
            + "    try:\n"
            + "        response = opener.open('http://127.0.0.1:18080' + path, timeout=8)\n"
            + "        return {'status': response.status, 'body': response.read().decode('utf-8', 'replace')}\n"
            + "    except urllib.error.HTTPError as error:\n"
            + "        return {'status': error.code, 'body': error.read().decode('utf-8', 'replace')}\n"
            + "    except Exception as error:\n"
            + "        return {'status': None, 'body': type(error).__name__ + ': ' + str(error)}\n"
            + "results = {path: get(path) for path in ('/setup', '/', '/api/status')}\n"
            + "print('HTTP_RESULTS=' + json.dumps(results, separators=(',', ':')))\n"
            + "PY\nprobe_rc=$?\n"
            + "wait \"$EP\"\nemu_rc=$?\ntrap - EXIT INT TERM\n"
            + duration_write
            + "if [ \"$probe_rc\" -ne 0 ]; then exit \"$probe_rc\"; fi\n"
            + "exit \"$emu_rc\"\n"
        )
    return (
        common
        + "start_s=$(date +%s)\n"
        + f"{command}\nemu_rc=$?\n"
        + duration_write
        + "exit \"$emu_rc\"\n"
    )


def parse_scenarios(value: str) -> list[str]:
    if value.strip().lower() == "all":
        return list(SCENARIOS)
    scenarios = [item.strip().upper() for item in value.split(",") if item.strip()]
    invalid = [item for item in scenarios if item not in EVALUATORS]
    if not scenarios or invalid:
        raise ValueError(f"unknown scenario selection: {value!r}")
    if len(set(scenarios)) != len(scenarios):
        raise ValueError("scenario selection contains duplicates")
    return scenarios


def _json_marker(output: str, prefix: str) -> dict[str, Any] | None:
    for line in output.splitlines():
        if line.startswith(prefix):
            try:
                value = json.loads(line[len(prefix) :])
            except json.JSONDecodeError:
                return None
            return value if isinstance(value, dict) else None
    return None


def _run_scenario(
    scenario: str,
    *,
    emu: str,
    image: Path,
    elf: Path,
    out_dir: Path,
) -> tuple[str, dict[str, Any]]:
    log_path = out_dir / f"{scenario.lower()}.log"
    script_path = out_dir / f"{scenario.lower()}.sh"
    repo_wsl = to_wsl_path(REPO_ROOT)
    out_wsl = to_wsl_path(out_dir.resolve())
    script = build_scenario_script(
        scenario,
        repo_wsl,
        emu,
        to_wsl_path(image.resolve()),
        to_wsl_path(elf.resolve()),
        to_wsl_path(log_path.resolve()),
        out_wsl,
    )
    script_path.write_bytes(script.encode("utf-8"))
    _, _, outer_seconds = build_emulator_argv(
        scenario, emu, to_wsl_path(image.resolve()), to_wsl_path(elf.resolve())
    )
    duration_path = out_dir / f"{scenario.lower()}.duration"
    duration_path.write_text("", encoding="utf-8")
    started = time.monotonic()
    try:
        result = subprocess.run(
            build_wsl_argv(to_wsl_path(script_path.resolve())),
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=outer_seconds + 20,
            check=False,
        )
        elapsed = time.monotonic() - started
        extra: dict[str, Any] = {
            "host_wall_seconds": round(elapsed, 3),
            "wsl_returncode": result.returncode,
        }
        if result.stdout.strip():
            extra["wsl_stdout"] = result.stdout.strip()[-4000:]
        if result.stderr.strip():
            extra["wsl_stderr"] = result.stderr.strip()[-2000:]
        if scenario == "S2":
            extra["control"] = _json_marker(result.stdout, "CONTROL_RESULTS=")
        elif scenario == "S4":
            extra["http"] = _json_marker(result.stdout, "HTTP_RESULTS=")
    except (OSError, subprocess.TimeoutExpired) as error:
        elapsed = time.monotonic() - started
        extra = {"host_wall_seconds": round(elapsed, 3), "run_error": str(error)}
    try:
        duration_text = duration_path.read_text(encoding="utf-8").strip()
        duration_match = re.search(r"EMU_DURATION_SECONDS=([0-9]+(?:\.[0-9]+)?)", duration_text)
        if duration_match:
            extra["duration_seconds"] = float(duration_match.group(1))
        else:
            extra.setdefault("run_error", "WSL did not record emulator runtime")
    except OSError:
        extra.setdefault("run_error", "WSL did not record emulator runtime")
    try:
        log_text = log_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        log_text = ""
    return log_text, extra


def _resolve_path(value: str) -> Path:
    path = Path(value).expanduser()
    return path if path.is_absolute() else REPO_ROOT / path


def _parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--scenario",
        default="all",
        metavar="S1,S2,...|all",
        help="comma-separated scenarios to run (default: all)",
    )
    parser.add_argument("--emu", default=DEFAULT_EMU, help="esp-emu path inside WSL")
    parser.add_argument("--image", default=DEFAULT_IMAGE, help="merged firmware image")
    parser.add_argument("--elf", default=DEFAULT_ELF, help="emulator-profile ELF")
    parser.add_argument("--out-dir", default=DEFAULT_OUT_DIR, help="logs and JSON report directory")
    parser.add_argument(
        "--build-image",
        action="store_true",
        help="merge the 16 MB flash image from .test-build/idf-emulator",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(argv)
    try:
        scenarios = parse_scenarios(args.scenario)
    except ValueError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 2

    image = _resolve_path(args.image).resolve()
    elf = _resolve_path(args.elf).resolve()
    out_dir = _resolve_path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    setup_error = None
    if args.build_image:
        try:
            build_merged_image(REPO_ROOT / ".test-build" / "idf-emulator", image)
        except (OSError, ValueError, json.JSONDecodeError) as error:
            setup_error = f"could not build merged image: {error}"
    elif not image.is_file():
        setup_error = f"firmware image does not exist: {image}"
    if not elf.is_file():
        setup_error = setup_error or f"firmware ELF does not exist: {elf}"

    report: dict[str, Any] = {
        "scenarios": {},
        "image": str(image),
        "elf": str(elf),
        "limitations": LIMITATIONS,
    }
    all_passed = True
    for scenario in scenarios:
        if setup_error:
            log_text, extra = "", {"run_error": setup_error}
        else:
            log_text, extra = _run_scenario(
                scenario, emu=args.emu, image=image, elf=elf, out_dir=out_dir
            )
        ok, reasons = EVALUATORS[scenario](log_text, extra)
        all_passed = all_passed and ok
        report["scenarios"][scenario] = {
            "result": "PASS" if ok else "FAIL",
            "reasons": reasons,
            "log": str(out_dir / f"{scenario.lower()}.log"),
            "extra": extra,
        }
        print(f"{scenario}: {'PASS' if ok else 'FAIL'}")
        for reason in reasons:
            print(f"  - {reason}")

    report_path = out_dir / "report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Report: {report_path}")
    print(f"Limits: {LIMITATIONS}")
    return 0 if all_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
