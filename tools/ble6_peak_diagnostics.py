"""Inspect the installed Nordic peak selection on actual paired PCT input."""
import json
import os
import struct
import subprocess
import sys
import ble6_research_replay as replay


def diagnose(case, size):
    work = replay.BUILD / f"peak-diagnostics-{size}"
    results, source = replay.build_and_replay([case], workdir=work, nfft=size)
    manifest = {"version": 1, "tests": [{"name": "ble6_peak_diagnostics",
        "sources": ["tests/host/ble6_peak_diagnostics.c", "nRF54L15/src/nrf_cs_measurement_quality.c"],
        "include_dirs": [work.relative_to(replay.ROOT).as_posix(), "nRF54L15/include"], "libraries": ["m"]}]}
    path = work / "peak-manifest.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")
    profile_path = work / "profile.bin"
    env = dict(os.environ, BLE6_REPLAY_INPUT=str(work / "input.bin"), BLE6_PEAK_PROFILE=str(profile_path))
    run = subprocess.run([sys.executable, "tools/run_host_tests.py", "--manifest", str(path),
                          "--build-dir", str(work / "peak-native")], cwd=replay.ROOT,
                         capture_output=True, text=True, encoding="utf-8", errors="replace", env=env)
    if run.returncode:
        raise RuntimeError(run.stdout + run.stderr)
    output = json.loads((work / "peak-native/host-test-report.json").read_text(encoding="utf-8"))
    trace = json.loads(output["results"][0]["stdout"])
    trace["profile"] = list(struct.unpack(f"<{size}f", profile_path.read_bytes()))
    trace["replayed_distance_m"] = results[0][1]
    trace["sdk_sha256"] = replay.sha256(source)
    return trace


def main():
    case = next(c for c in replay.wieme_cases()
                if c["file"] == "channel_map/NLOS/50_spread.csv" and c["row"] == 19)
    result = {"file": case["file"], "row": case["row"], "truth_m": case["truth_m"],
              "traces": {str(n): diagnose(case, n) for n in (512, 1024, 2048)}}
    path = replay.BUILD / "peak-diagnostics.json"
    path.write_text(json.dumps(result, indent=2, allow_nan=False), encoding="utf-8")
    print("REPORT", path)


if __name__ == "__main__":
    main()
