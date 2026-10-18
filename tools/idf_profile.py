"""Resolve isolated ESP-IDF build profiles and write profile build evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
from pathlib import Path
from typing import Iterable


OPTIMIZATION_KEYS = (
    "CONFIG_COMPILER_OPTIMIZATION_DEBUG",
    "CONFIG_COMPILER_OPTIMIZATION_PERF",
)
PROFILE_KEYS = {
    "debug": "CONFIG_COMPILER_OPTIMIZATION_DEBUG",
    "performance": "CONFIG_COMPILER_OPTIMIZATION_PERF",
}
PROFILE_DEFAULTS = {
    "emulator": "sdkconfig.emulator.defaults",
}
PROFILE_NAMES = tuple(PROFILE_KEYS) + tuple(PROFILE_DEFAULTS)
EMULATOR_SETTINGS = {
    "CONFIG_APP_DISPLAY_ENABLED": False,
    "CONFIG_APP_KEYPAD_ENABLED": False,
    "CONFIG_SEISMIC_SOURCE_TEST": True,
}
_ASSIGNMENT = re.compile(r"^CONFIG_([A-Z0-9_]+)=(.*)$")
_UNSET = re.compile(r"^#\s+CONFIG_([A-Z0-9_]+)\s+is not set$")
_CMAKE_DEFAULTS = re.compile(
    r"set\(\s*SDKCONFIG_DEFAULTS\s+\"([^\"]+)\"\s*\)", re.DOTALL
)
_CONDITIONAL_DEFAULTS = (
    ("GEOLOCATOR_INSIGHTS_PROFILE", "sdkconfig.insights.defaults"),
    ("GEOLOCATOR_KEYPAD_TIMING_PROFILE", "sdkconfig.keypad-timing.defaults"),
    ("GEOLOCATOR_EMULATOR_PROFILE", "sdkconfig.emulator.defaults"),
)


def _parse_value(value: str) -> object:
    if value == "y":
        return True
    if value == "n":
        return False
    return value


def parse_config_fragments(paths: Iterable[Path]) -> dict[str, object]:
    values: dict[str, object] = {}
    for path in paths:
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except OSError as error:
            raise ValueError(f"configuration fragment is unavailable: {path}") from error
        for line in lines:
            unset = _UNSET.match(line.strip())
            if unset:
                values["CONFIG_" + unset.group(1)] = False
                continue
            assignment = _ASSIGNMENT.match(line.strip())
            if assignment:
                values["CONFIG_" + assignment.group(1)] = _parse_value(
                    assignment.group(2)
                )
    return values


def _split_defaults(value: str | Iterable[str | Path] | None) -> list[str | Path]:
    if value is None:
        return []
    if isinstance(value, str):
        return [entry for entry in value.split(";") if entry]
    return list(value)


def _cmake_default_paths(root: Path) -> list[Path]:
    cmake_lists = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    match = _CMAKE_DEFAULTS.search(cmake_lists)
    names = match.group(1).split(";") if match else ["sdkconfig.defaults"]
    for variable, fragment in _CONDITIONAL_DEFAULTS:
        if os.environ.get(variable) == "1":
            names.append(fragment)
    paths = [(root / name).resolve() for name in names]
    return list(dict.fromkeys(paths))


def resolve_profile(
    profile: str,
    project_root: str | Path,
    *,
    inherited_defaults: str | Iterable[str | Path] | None = None,
    profile_defaults: str | Path | None = None,
) -> dict[str, object]:
    """Return deterministic config/build paths after validating a profile."""

    profile = profile.lower()
    if profile not in PROFILE_NAMES:
        raise ValueError(f"unsupported ESP-IDF build profile: {profile}")

    root = Path(project_root).resolve()
    project_defaults = _cmake_default_paths(root)
    inherited = [
        (Path(path) if Path(path).is_absolute() else root / Path(path)).resolve()
        for path in _split_defaults(inherited_defaults)
    ]
    if profile_defaults is None:
        default_name = PROFILE_DEFAULTS.get(
            profile,
            f"sdkconfig.profile-{profile}.defaults",
        )
        profile_defaults = root / default_name
        if not Path(profile_defaults).exists():
            repository_root = Path(__file__).resolve().parent.parent
            profile_defaults = repository_root / default_name
    profile_path = Path(profile_defaults)
    if not profile_path.is_absolute():
        profile_path = root / profile_path
    profile_path = profile_path.resolve()
    resolved_defaults = project_defaults + [
        path for path in inherited if path not in project_defaults and path != profile_path
    ]
    if profile_path not in resolved_defaults:
        resolved_defaults.append(profile_path)

    values = parse_config_fragments(resolved_defaults)
    if profile in PROFILE_KEYS:
        selected = PROFILE_KEYS[profile]
        other = next(key for key in OPTIMIZATION_KEYS if key != selected)
        if values.get(selected) is not True or values.get(other) is not False:
            raise ValueError(
                f"resolved defaults do not resolve to {profile}: "
                f"{selected}={values.get(selected)!r}, {other}={values.get(other)!r}"
            )
        optimization = selected.removeprefix("CONFIG_COMPILER_OPTIMIZATION_").lower()
        settings = {key: values.get(key) for key in OPTIMIZATION_KEYS}
        build_dir = (root / f"build-profile-{profile}").resolve()
    else:
        mismatches = {
            key: values.get(key)
            for key, expected in EMULATOR_SETTINGS.items()
            if values.get(key) is not expected
        }
        if mismatches:
            raise ValueError(
                f"resolved defaults do not resolve to emulator: {mismatches}"
            )
        optimization = None
        settings = dict(EMULATOR_SETTINGS)
        build_dir = (root / ".test-build" / "idf-emulator").resolve()

    standard_build = (root / "build").resolve()
    if build_dir == standard_build or standard_build in build_dir.parents:
        raise ValueError("profile build directory must stay outside build/")
    return {
        "project_root": str(root),
        "profile": profile,
        "optimization": optimization,
        "settings": settings,
        "sdkconfig_defaults": [str(path) for path in resolved_defaults],
        "cmake_profile_defaults": [
            str(path) for path in resolved_defaults[len(project_defaults) :]
        ],
        "build_dir": str(build_dir),
        "sdkconfig": str(build_dir / "sdkconfig"),
        "cmake_include": str(build_dir / "profile-sdkconfig-defaults.cmake"),
        "evidence_json": str(build_dir / f"build-evidence.{profile}.json"),
        "size_report": str(build_dir / "idf-size.txt"),
        "size_components_report": str(build_dir / "idf-size-components.txt"),
    }


def _cmake_literal(value: str) -> str:
    equals = "="
    while f"]{equals}]" in value:
        equals += "="
    return f"[{equals}[{value}]{equals}]"


def write_profile_cmake_include(resolved: dict[str, object]) -> Path:
    """Append profile inputs after the project's existing defaults list."""

    root = Path(str(resolved["project_root"]))
    include_path = Path(str(resolved["cmake_include"]))
    lines = [
        "# Generated by tools/idf_profile.py; do not edit.",
        f"if(CMAKE_SOURCE_DIR STREQUAL {_cmake_literal(root.as_posix())})",
    ]
    for default_path in resolved["cmake_profile_defaults"]:
        cmake_path = str(default_path).replace("\\", "/")
        lines.append(
            f"    list(APPEND SDKCONFIG_DEFAULTS {_cmake_literal(cmake_path)})"
        )
    lines.extend(("endif()", ""))
    include_path.parent.mkdir(parents=True, exist_ok=True)
    include_path.write_text("\n".join(lines), encoding="utf-8")
    return include_path


def _find_firmware(build_dir: Path) -> Path:
    excluded = {"bootloader.bin", "partition-table.bin", "ota_data_initial.bin"}
    candidates = sorted(
        path
        for path in build_dir.glob("*.bin")
        if path.name.lower() not in excluded
    )
    if len(candidates) != 1:
        names = ", ".join(path.name for path in candidates) or "none"
        raise ValueError(
            f"expected one application firmware .bin in {build_dir}, found: {names}"
        )
    return candidates[0]


def write_build_evidence(
    profile: str,
    project_root: str | Path,
    firmware: str | Path | None = None,
    *,
    inherited_defaults: str | Iterable[str | Path] | None = None,
) -> Path:
    resolved = resolve_profile(
        profile, project_root, inherited_defaults=inherited_defaults
    )
    build_dir = Path(str(resolved["build_dir"]))
    firmware_path = _find_firmware(build_dir) if firmware is None else Path(firmware)
    if not firmware_path.is_absolute():
        firmware_path = Path(project_root).resolve() / firmware_path
    firmware_path = firmware_path.resolve()
    if not firmware_path.is_file() or build_dir not in firmware_path.parents:
        raise ValueError(f"firmware must be a file inside the profile build: {firmware_path}")

    digest = hashlib.sha256(firmware_path.read_bytes()).hexdigest()
    evidence_path = Path(str(resolved["evidence_json"]))
    evidence = {
        "profile": resolved["profile"],
        "build_dir": str(build_dir),
        "sdkconfig": resolved["sdkconfig"],
        "sdkconfig_defaults": resolved["sdkconfig_defaults"],
        "firmware": firmware_path.name,
        "firmware_sha256": digest,
    }
    evidence_path.parent.mkdir(parents=True, exist_ok=True)
    evidence_path.write_text(
        json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return evidence_path


def _cli() -> int:
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("resolve")
    commands.add_parser("prepare-cmake")
    record_parser = commands.add_parser("record-evidence")
    for command in commands.choices.values():
        command.add_argument("--profile", choices=PROFILE_NAMES, required=True)
        command.add_argument("--project", type=Path, required=True)
        command.add_argument("--inherited-defaults")
    record_parser.add_argument("--firmware", type=Path)
    args = parser.parse_args()

    try:
        if args.command in {"resolve", "prepare-cmake"}:
            result = resolve_profile(
                args.profile,
                args.project,
                inherited_defaults=args.inherited_defaults,
            )
            if args.command == "prepare-cmake":
                print(write_profile_cmake_include(result))
            else:
                print(json.dumps(result, sort_keys=True))
        else:
            path = write_build_evidence(
                args.profile,
                args.project,
                args.firmware,
                inherited_defaults=args.inherited_defaults,
            )
            print(path)
    except (OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(_cli())
