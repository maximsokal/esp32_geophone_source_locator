"""Counterfactual setup reuse cost with unchanged production coordinator/runtime/replay.

This models latency only; it does not implement a persistent BLE connection.
"""
from __future__ import annotations
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import analyze_nrf_cs_trace as trace

ROOT=Path(__file__).resolve().parents[1]
BUILD=ROOT/'.test-build/ble6-connection-reuse'
SETUP_STAGES=('CONNECTED','SECURITY_READY','MTU_READY','DISCOVERY_READY','FEATURES_READY',
              'CAPABILITIES_READY','CONFIG_READY','CS_SECURITY_READY')


def extract_profiles(text,name):
    seen=set()
    for line in text.splitlines():
        match=trace.LINE.match(line.strip())
        if match and match[3].startswith('execute'):
            key=int(match[2])
            if key in seen:raise ValueError(f'Duplicate execute id in {name}: {key}')
            seen.add(key)
    attempts,_=trace.parse(text)
    rows=[]
    for key,a in attempts.items():
        stages=dict(a.events)
        required=('EXECUTE_WORK_STARTED',)+SETUP_STAGES
        if a.result!='SUCCESS' or any(s not in stages for s in required):continue
        times=[stages[s] for s in required]
        if (a.total_ms is None or a.measured_ms is None or a.cleanup_ms is None or
            times!=sorted(times) or not 0<=times[0]<=times[-1]<=a.measured_ms<=a.total_ms or
            a.measured_ms+a.cleanup_ms!=a.total_ms):continue
        setup=times[-1]-times[0]
        if setup<=0 or a.total_ms<=setup:continue
        rows.append({'file':name,'attempt':key,'cold_ms':a.total_ms,'warm_ms':a.total_ms-setup,
                     'setup_ms':setup,'queue_ms':times[0],'cleanup_ms':a.cleanup_ms})
    return rows,len(attempts)-len(rows)


def select_profiles(rows):
    if not rows:raise ValueError('No complete successful timing profiles')
    ordered=sorted(rows,key=lambda r:(r['cold_ms'],r['file'],r['attempt']))
    return [ordered[max(0,math.ceil(q*len(ordered))-1)] for q in (.5,.95,.99)]


def native_model(profiles,work=BUILD):
    work.mkdir(parents=True,exist_ok=True)
    path=work/'profiles.txt';output=work/'model.csv'
    path.write_text(str(len(profiles))+'\n'+''.join(f"{p['cold_ms']} {p['warm_ms']}\n" for p in profiles))
    entry=json.loads((ROOT/'tests/host/manifest.d/ble-cs-acceptance.json').read_text())['tests'][0]
    entry['name']='ble6_connection_reuse_timing'
    entry['sources']=[s.replace('tests/host/nrf_cs_cycle_geometry_golden_test.c',
        'tests/host/ble6_connection_reuse_timing.c') for s in entry['sources']]
    manifest=work/'manifest.json';manifest.write_text(json.dumps({'version':1,'tests':[entry]}))
    run=subprocess.run([sys.executable,'tools/run_host_tests.py','--manifest',str(manifest),
        '--build-dir',str(work/'native')],cwd=ROOT,capture_output=True,
        env={**os.environ,'BLE6_REUSE_PROFILES':str(path),'BLE6_REUSE_OUTPUT':str(output)})
    (work/'run.log').write_bytes(run.stdout+run.stderr)
    if run.returncode:raise RuntimeError(f'Native reuse model failed: {work / "run.log"}')
    with output.open(newline='') as stream:
        return [{k:int(v) for k,v in row.items()} for row in csv.DictReader(stream)]


def main():
    paths=[ROOT/'logs/nrf'/f'nrf_COM3_2026-11-03_{t}.log' for t in ('00-03-29','16-45-51','16-59-11')]
    samples=[];excluded={}
    for path in paths:
        rows,skipped=extract_profiles(path.read_text(encoding='utf-8',errors='replace'),path.name)
        samples+=rows;excluded[path.name]=skipped
    selected=select_profiles(samples)
    rows=native_model(selected)
    report={'kind':'counterfactual_latency_only','samples':len(samples),'excluded':excluded,
        'profiles':dict(zip(('p50','p95','p99'),selected)),'model_rows':len(rows),
        'sha256':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},
        'limitations':['no adapter/HCI/connection cache implementation',
          'setup removal is an assumption, not measured warm-link latency',
          'queue wait and successful-attempt cleanup costs retained',
          'fault inputs and timing are synthetic; RF correlation not evaluated',
          'three-node cycles cannot meet recording geometry minimum']}
    (BUILD/'report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps(report,indent=2),flush=True)
    for row in rows:
        if row['nodes']==5 and row['rate']==100000 and row['scenario'] in (0,4):print(row)


if __name__=='__main__':main()
