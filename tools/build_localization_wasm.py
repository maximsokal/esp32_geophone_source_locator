#!/usr/bin/env python3
"""Build the firmware's source-localization run as WebAssembly (tools/wasm/localization_wasm.c).

The calculation is the firmware's own code: the sources below are the ones the host test of localization_calculate_run
builds, plus the JSON readers/writers of the records. Only the ESP-IDF headers are replaced by the host stubs and the
recording/geometry/run-store seam by tools/wasm/localization_wasm.c.

    python tools/build_localization_wasm.py [--output .test-build/wasm/localization.wasm] [--embed webui/src/wasm/localizationKernelBinary.generated.ts]

Needs a C compiler that targets wasm32-wasi; `pip install ziglang` provides one.
"""
from __future__ import annotations

import argparse
import json
import os
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get("IDF_PATH", "C:/Espressif/frameworks/esp-idf-v5.5.4"))

# The sources of the host test of the run, minus the test itself; the PSRAM model stands in for heap_caps.
MANIFEST = ROOT / "tests/host/manifest.d/coverage-wf7.json"
TEST_NAME = "localization_calculate_run"

EXTRA_SOURCES = [
    "components/localization_job/localization_config.c",
    "components/localization_results/localization_run_record_json.c",
    "components/localization_results/localization_pick_record.c",
    "components/object_store/object_store_geometry_json.c",
    "tools/wasm/localization_wasm.c",
]
EXTRA_INCLUDES = [
    "components/localization_job/include",
    "components/localization_results/include",
    "components/object_store/include",
    "components/seismic_recorder/include",
    "components/json_number/include",
]
# What the page may call; everything else stays internal.
EXPORTS = [
    "lw_alloc", "lw_free", "lw_reset", "lw_frame_size", "lw_frame_timestamp_offset", "lw_frame_channels_offset",
    "lw_set_recording", "lw_set_geometry_json", "lw_check_config", "lw_run", "lw_result_json", "lw_record_count",
    "lw_channel_health_flat", "lw_channel_health_identical",
]


def foreign_imports(module: bytes) -> list[str]:
    """Imports of the module that are not WASI calls. Every one of them is a function the build did not find: the linker
    turns it into a call that fails when the page runs it, which is how the missing material table went unnoticed."""

    def leb(data: bytes, at: int) -> tuple[int, int]:
        value = shift = 0
        while True:
            byte = data[at]
            at += 1
            value |= (byte & 0x7F) << shift
            if byte < 0x80:
                return value, at
            shift += 7

    at = 8
    names: list[str] = []
    while at < len(module):
        section, at = module[at], at + 1
        size, at = leb(module, at)
        end = at + size
        if section == 2:
            count, at = leb(module, at)
            for _ in range(count):
                length, at = leb(module, at)
                owner = module[at:at + length].decode()
                at += length
                length, at = leb(module, at)
                name = module[at:at + length].decode()
                at += length
                kind = module[at]
                at += 1
                if kind == 0:
                    _, at = leb(module, at)
                elif kind == 1:
                    at += 1
                    flags, at = leb(module, at)
                    _, at = leb(module, at)
                    if flags & 1:
                        _, at = leb(module, at)
                elif kind == 2:
                    flags, at = leb(module, at)
                    _, at = leb(module, at)
                    if flags & 1:
                        _, at = leb(module, at)
                else:
                    at += 2
                if owner != "wasi_snapshot_preview1":
                    names.append(f"{owner}.{name}")
        at = end
    return names


def sources_and_includes() -> tuple[list[str], list[str]]:
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    test = next(t for t in manifest["tests"] if t["name"] == TEST_NAME)
    sources = [s for s in test["sources"] if not s.endswith("_test.c") and "object_store_profile/cJSON.c" not in s]
    sources += [s for s in EXTRA_SOURCES if s not in sources]
    includes = list(test["include_dirs"]) + [i for i in EXTRA_INCLUDES if i not in test["include_dirs"]]
    return sources, includes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", default=str(ROOT / ".test-build/wasm/localization.wasm"))
    parser.add_argument("--zig", default=None, help="compiler command, default: this Python -m ziglang")
    parser.add_argument("--embed", default=None, metavar="TS_FILE",
                        help="also write the module as a base64 string for the page (the firmware only serves .js assets)")
    args = parser.parse_args()

    sources, includes = sources_and_includes()
    cjson = IDF / "components/json/cJSON"
    # The real cJSON header goes FIRST. The host test directories carry a trimmed cJSON.h whose struct is laid out
    # differently from the cJSON.c this module is built with: numbers and booleans still read, but every string and object
    # is misread, so any settings file naming a material or a velocity range was refused with ESP_ERR_INVALID_ARG
    # (board 2026-11-16: "the calculation stopped with error 258").
    include_flags = [f"-I{cjson}"] + [f"-I{ROOT / i}" for i in includes]
    zig = shlex.split(args.zig) if args.zig else [sys.executable, "-m", "ziglang"]
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    command = zig + ["cc", "-target", "wasm32-wasi", "-mexec-model=reactor", "-O2", "-std=gnu11", "-Wno-everything",
                     "-DCJSON_HIDE_SYMBOLS", "-DCONFIG_LOCALIZATION_MAX_ANALYSIS_S=10"]
    command += include_flags
    command += [str(ROOT / s) for s in sources] + [str(cjson / "cJSON.c")]
    command += [f"-Wl,--export={name}" for name in EXPORTS]
    command += ["-Wl,--export-memory", "-Wl,--initial-memory=33554432", "-Wl,--max-memory=1073741824",
                "-Wl,-z,stack-size=1048576", "-s", "-o", str(output)]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stderr[-6000:])
        return result.returncode
    print(f"wrote {output} ({output.stat().st_size} bytes)")
    missing = foreign_imports(output.read_bytes())
    if missing:
        sys.stderr.write("the module imports functions the build did not find: " + ", ".join(missing) + "\n")
        return 1
    if args.embed:
        import base64
        embed = Path(args.embed)
        embed.parent.mkdir(parents=True, exist_ok=True)
        text = base64.b64encode(output.read_bytes()).decode("ascii")
        lines = [
            "// Generated by tools/build_localization_wasm.py --embed: the firmware's source-localization run as WebAssembly.",
            "// Do not edit; rebuild with `npm run build:wasm` after changing the calculation code.",
            f'export const localizationKernelBase64 = "{text}";',
            "",
        ]
        embed.write_text("\n".join(lines), encoding="utf-8", newline="\n")
        print(f"wrote {embed} ({embed.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
