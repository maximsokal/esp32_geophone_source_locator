import hashlib
import json
import subprocess
import sys
from pathlib import Path
from shutil import copyfile, which

import pytest

from tools.idf_profile import (
    resolve_profile,
    write_build_evidence,
    write_profile_cmake_include,
)


REPO_ROOT = Path(__file__).resolve().parents[2]
PROFILE_RESOLVER = REPO_ROOT / "tools" / "idf_profile.py"


@pytest.mark.parametrize(
    ("profile", "selected", "unselected"),
    [
        ("debug", "CONFIG_COMPILER_OPTIMIZATION_DEBUG", "CONFIG_COMPILER_OPTIMIZATION_PERF"),
        ("performance", "CONFIG_COMPILER_OPTIMIZATION_PERF", "CONFIG_COMPILER_OPTIMIZATION_DEBUG"),
    ],
)
def test_profile_resolves_the_requested_optimization(profile, selected, unselected):
    resolved = resolve_profile(profile, REPO_ROOT)

    assert resolved["settings"][selected] is True
    assert resolved["settings"][unselected] is False


def test_wrong_profile_defaults_fixture_is_rejected(tmp_path):
    wrong_defaults = tmp_path / "sdkconfig.wrong-profile.defaults"
    wrong_defaults.write_text(
        "CONFIG_COMPILER_OPTIMIZATION_PERF=y\n"
        "# CONFIG_COMPILER_OPTIMIZATION_DEBUG is not set\n",
        encoding="utf-8",
    )

    with pytest.raises(ValueError, match="resolved defaults do not resolve to debug"):
        resolve_profile("debug", REPO_ROOT, profile_defaults=wrong_defaults)


def test_clean_profile_resolution_metadata_is_deterministic():
    first = resolve_profile("performance", REPO_ROOT)
    second = resolve_profile("performance", REPO_ROOT)

    assert first == second
    assert first["profile"] == "performance"
    assert first["build_dir"] == str(REPO_ROOT / "build-profile-performance")
    assert first["sdkconfig"] == str(REPO_ROOT / "build-profile-performance" / "sdkconfig")
    assert first["size_report"] == str(REPO_ROOT / "build-profile-performance" / "idf-size.txt")
    assert first["size_components_report"] == str(
        REPO_ROOT / "build-profile-performance" / "idf-size-components.txt"
    )


def test_wrapper_resolution_cli_uses_the_same_defaults_and_isolated_sdkconfig():
    result = subprocess.run(
        [
            sys.executable,
            str(PROFILE_RESOLVER),
            "resolve",
            "--profile",
            "performance",
            "--project",
            str(REPO_ROOT),
            "--inherited-defaults",
            "sdkconfig.lvgl.defaults",
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    resolved = json.loads(result.stdout)
    wrapper = (REPO_ROOT / "tools" / "run_idf.ps1").read_text(encoding="utf-8")

    assert resolved == resolve_profile(
        "performance", REPO_ROOT, inherited_defaults="sdkconfig.lvgl.defaults"
    )
    assert resolved["sdkconfig_defaults"] == [
        str(REPO_ROOT / "sdkconfig.defaults"),
        str(REPO_ROOT / "sdkconfig.storage.defaults"),
        str(REPO_ROOT / "sdkconfig.lvgl.defaults"),
        str(REPO_ROOT / "sdkconfig.tls.defaults"),
        str(REPO_ROOT / "sdkconfig.button.defaults"),
        str(REPO_ROOT / "sdkconfig.profile-performance.defaults"),
    ]
    assert '"prepare-cmake"' in wrapper and '"--profile"' in wrapper
    assert '$env:SDKCONFIG = $sdkconfigPath' in wrapper
    assert '"CMAKE_PROJECT_INCLUDE_BEFORE=$profileCmakeInclude"' in wrapper


def test_profile_cmake_include_appends_to_existing_project_defaults(tmp_path):
    profile_project = tmp_path / "profile-project"
    profile_project.mkdir()
    for name in (
        "CMakeLists.txt",
        "sdkconfig.defaults",
        "sdkconfig.storage.defaults",
        "sdkconfig.lvgl.defaults",
        "sdkconfig.tls.defaults",
        "sdkconfig.button.defaults",
        "sdkconfig.profile-performance.defaults",
    ):
        copyfile(REPO_ROOT / name, profile_project / name)

    resolved = resolve_profile("performance", profile_project)
    include_path = write_profile_cmake_include(resolved)
    include_text = include_path.read_text(encoding="utf-8")

    assert "list(APPEND SDKCONFIG_DEFAULTS" in include_text
    assert (profile_project / "sdkconfig.profile-performance.defaults").as_posix() in include_text
    assert (profile_project / "sdkconfig.defaults").as_posix() not in include_text


def test_build_evidence_records_profile_and_firmware_sha(tmp_path):
    for name in (
        "CMakeLists.txt",
        "sdkconfig.defaults",
        "sdkconfig.storage.defaults",
        "sdkconfig.lvgl.defaults",
        "sdkconfig.tls.defaults",
        "sdkconfig.button.defaults",
        "sdkconfig.profile-performance.defaults",
    ):
        copyfile(REPO_ROOT / name, tmp_path / name)
    build_dir = tmp_path / "build-profile-performance"
    build_dir.mkdir()
    firmware = build_dir / "geolocator.bin"
    firmware_bytes = b"synthetic firmware image for evidence contract"
    firmware.write_bytes(firmware_bytes)

    evidence_path = write_build_evidence("performance", tmp_path)
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))

    assert evidence["profile"] == "performance"
    assert evidence["firmware"] == firmware.name
    assert evidence["firmware_sha256"] == hashlib.sha256(firmware_bytes).hexdigest()


def test_invoke_checked_logs_stderr_and_checks_exit_code(tmp_path):
    powershell = which("powershell.exe") or which("powershell")
    if powershell is None:
        pytest.skip("Windows PowerShell is required")

    wrapper = (REPO_ROOT / "tools" / "run_idf.ps1").read_text(encoding="utf-8")
    start = wrapper.index("function Invoke-Checked(")
    end = wrapper.index("\n}", start) + 2
    success_log = tmp_path / "stderr-success.log"
    failure_log = tmp_path / "stderr-failure.log"
    quote = lambda value: "'" + str(value).replace("'", "''") + "'"
    harness = tmp_path / "invoke-checked.ps1"
    harness.write_text(
        "$ErrorActionPreference = 'Stop'\n"
        + wrapper[start:end]
        + "\n$successArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', \"[Console]::Error.WriteLine('stderr-success'); exit 0\")\n"
        + "$failureArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', 'exit 1')\n"
        + f"$successLog = {quote(success_log)}\n$failureLog = {quote(failure_log)}\n"
        + f"Invoke-Checked {quote(powershell)} $successArgs $successLog\n"
        + "if (-not (Select-String -Path $successLog -SimpleMatch -Pattern 'stderr-success' -Quiet)) { exit 32 }\n"
        + "$threw = $false\ntry { Invoke-Checked "
        + quote(powershell)
        + " $failureArgs $failureLog } catch { if ($_ -match 'exit code 1') { $threw = $true } }\n"
        + "if (-not $threw) { exit 33 }\nexit 0\n",
        encoding="utf-8",
    )

    result = subprocess.run(
        [powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(harness)],
        capture_output=True,
        text=True,
    )

    assert result.returncode == 0, result.stdout + result.stderr
