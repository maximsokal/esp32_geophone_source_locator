"""Inspect the installed Nordic peak selection on actual paired PCT input."""
import json
import os
import struct
import subprocess
import sys
import math
import ble6_research_replay as replay
from ble6_series_benchmark import metrics


def confidence_study():
    """Compare the same confidence function with strongest/selected-path evidence, no fitted offset."""
    cases = list(replay.ocm_cases()) + list(replay.wieme_cases())
    for name in ('owl-indoor.parquet','owl-outdoor.parquet'):
        cases.extend(replay.owl_cases(replay.OWL/name,200))
    work = replay.BUILD/'selected-confidence'
    results,source = replay.build_and_replay(cases,workdir=work)
    extra = replay.confidence_extra(work)
    peak = list(struct.iter_unpack('<5f',(work/'peak.bin').read_bytes()))
    assert len(cases)==len(results)==len(extra)==len(peak)
    groups,examples = {},[]
    for case,result,tail,path in zip(cases,results,extra,peak):
        best = result[1]
        if not math.isfinite(best) or best<0 or case['truth_m'] is None:
            continue
        error = best-case['truth_m']
        policies = {'plain':tail[1],'production':tail[2],'selected_peak':path[4]}
        keys = [case['dataset']]
        if case['dataset']=='Wieme':
            keys.append('Wieme/NLOS' if 'NLOS' in case['file'].split('/') else 'Wieme/LOS')
        for key in keys:
            bucket = groups.setdefault(key,{'all':[],'plain':[],'production':[],'selected_peak':[],
                'removed_good_1m':0,'removed_bad_2m':0})
            bucket['all'].append(error)
            for policy,confidence in policies.items():
                if confidence>=500: bucket[policy].append(error)
            removed = tail[2]>=500 and path[4]<500
            bucket['removed_good_1m'] += removed and abs(error)<=1
            bucket['removed_bad_2m'] += removed and abs(error)>2
        if tail[2]>=500 and path[4]<500 and len(examples)<30:
            examples.append({k:case[k] for k in ('dataset','file','row','truth_m')} |
                {'estimate_m':best,'error_m':error,'global_ratio':tail[0],
                 'selected_ratio':path[3],'production':tail[2],'selected_confidence':path[4]})
    for bucket in groups.values():
        for policy in ('all','plain','production','selected_peak'):
            errors = bucket[policy]
            bucket[policy] = metrics(errors) | {'good_1m':sum(abs(e)<=1 for e in errors)}
        n = bucket['all']['n']
        for policy in ('plain','production','selected_peak'):
            bucket[policy]['coverage'] = bucket[policy]['n']/n
    report = {'cases':len(cases),'groups':groups,'examples':examples,
        'sdk_sha256':replay.sha256(source),'offset_m':0,
        'limitations':['same previously inspected datasets, not new holdout','host FFT, not target RF',
            'no complete synchronous geometry in single-link datasets','distance estimates unchanged']}
    (work/'report.json').write_text(json.dumps(report,indent=2,allow_nan=False),encoding='utf-8')
    print(json.dumps(groups,indent=2),flush=True)
    print('REPORT',work/'report.json')


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
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--confidence',action='store_true',help='compare strongest and selected-path confidence on public IQ')
    args = parser.parse_args()
    confidence_study() if args.confidence else main()
