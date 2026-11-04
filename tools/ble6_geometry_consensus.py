"""Diagnostic subset/order search using the production geometry solver, with synthetic truth kept outside native selection."""
from __future__ import annotations
import csv
import json
import math
import os
import subprocess
import sys
import random
import time
import hashlib
from pathlib import Path
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / '.test-build/ble6-geometry-consensus'
REFERENCE = [[0,0,0],[4000,0,0],[500,3500,0],[4300,3100,0],[1800,1600,0]]
TOLERANCE = 750


def make_case(name, points, errors=None, manual_mask=0, missing_mask=0):
    errors = errors or {}
    if len(points) not in (4,5) or list(points[0]) != [0,0,0]:
        raise ValueError('Four/five nodes including MAIN at origin required')
    pairs = []
    for a in range(len(points)):
        for b in range(a+1,len(points)):
            p = len(pairs)
            pairs.append({'distance': round(math.dist(points[a],points[b])) + errors.get(p,0),
                'spread': 33, 'flags': 64 if manual_mask & (1 << p) else 0,
                'state': 4 if missing_mask & (1 << p) else 2})
    return {'name': name, 'truth': points, 'z': [p[2] for p in points], 'cables': [0]+[20000]*(len(points)-1),
        'pairs': pairs, 'bad_mask': sum(1 << p for p,v in errors.items() if v), 'class': 'unspecified'}


def native_solutions(cases, workdir, solver_source='components/nrf_geometry_solver/nrf_geometry_solver.c'):
    workdir.mkdir(parents=True,exist_ok=True)
    input_path, output = workdir/'input.txt', workdir/'output.csv'
    with input_path.open('w',encoding='ascii') as stream:
        stream.write(f'{len(cases)}\n')
        for case in cases:
            n = len(case['truth'])
            stream.write(f'{n}\n')
            for i in range(5):
                stream.write(f"{case['z'][i] if i<n else 0} {case['cables'][i] if i<n else 0}\n")
            for i in range(10):
                p = case['pairs'][i] if i<len(case['pairs']) else {'distance':0,'spread':0,'flags':0,'state':0}
                stream.write(f"{p['distance']} {p['spread']} {p['flags']} {p['state']}\n")
    entry = {'name':'ble6_geometry_orders','sources':['tests/host/ble6_geometry_orders.c',
        str(solver_source),'components/nrf_geometry_solver/nrf_geometry_triangles.c'],
        'include_dirs':['components/nrf_geometry_solver/include','nRF54L15/include'],'libraries':['m'],
        'defines':['NRF_GEOMETRY_SOLVER_HOST_TEST=1']}
    manifest = workdir/'manifest.json'
    manifest.write_text(json.dumps({'version':1,'tests':[entry]}),encoding='utf-8')
    run = subprocess.run([sys.executable,'tools/run_host_tests.py','--manifest',str(manifest.relative_to(ROOT)),
        '--build-dir',str((workdir/'native').relative_to(ROOT))],cwd=ROOT,capture_output=True,
        env={**os.environ,'BLE6_GEOMETRY_INPUT':str(input_path),'BLE6_GEOMETRY_OUTPUT':str(output)})
    (workdir/'run.log').write_bytes(run.stdout+run.stderr)
    if run.returncode:
        raise RuntimeError(f'Native geometry diagnostics failed: {workdir / "run.log"}')
    results = [{'baseline':None,'solutions':[]} for _ in cases]
    with output.open(newline='',encoding='utf-8') as stream:
        for raw in csv.reader(stream):
            r = list(map(int,raw))
            n = len(cases[r[0]]['truth'])
            solution = {'mask':r[1],'order':r[2],'ok':bool(r[3]),'residual_mm':r[5],
                'nodes':[r[6+4*i:9+4*i] for i in range(n)],'uncertainty_mm':[r[9+4*i] for i in range(n)]}
            if r[4]: results[r[0]]['baseline'] = solution
            else: results[r[0]]['solutions'].append(solution)
    assert all(r['baseline'] is not None for r in results)
    return results


def shape_disagreement(solutions):
    if len(solutions)<2:
        return 0.0
    distances = [[math.dist(a,b) for i,a in enumerate(s['nodes']) for b in s['nodes'][i+1:]] for s in solutions]
    return float(np.ptp(np.asarray(distances),axis=0).max())


def node_error(solution, truth):
    predicted, reference = np.asarray(solution['nodes'],dtype=float), np.asarray(truth,dtype=float)
    u,_,vt = np.linalg.svd(predicted[:,:2].T @ reference[:,:2])
    delta = predicted[:,:2] @ u @ vt - reference[:,:2]
    errors = np.sqrt(np.sum(delta*delta,axis=1)+(predicted[:,2]-reference[:,2])**2)
    return {'nodes_mm':errors.tolist(),'max_mm':float(max(errors))}


def residual_scaled_uncertainty(case, solution):
    """Research proxy for relative-sigma covariance; uses rounded output, never true XY."""
    n = len(solution['nodes'])
    chi_square, edges = 0.0, 0
    for p, (a,b) in enumerate((a,b) for a in range(n) for b in range(a+1,n)):
        pair = case['pairs'][p]
        if pair['state'] != 2 or solution['mask'] & (1 << p):
            continue
        residual = math.dist(solution['nodes'][a],solution['nodes'][b]) - pair['distance']
        chi_square += (residual / max(150,pair['spread']))**2
        edges += 1
    degrees = edges - (2*n-3)
    if degrees <= 0:
        return None
    scale = math.sqrt(max(1.0,chi_square/degrees))
    return [u*scale for u in solution['uncertainty_mm']]


def full_fit_cost(case, solution):
    """The production Huber objective evaluated on rounded coordinates, not true positions."""
    cost = 0.0
    n = len(solution['nodes'])
    for p,(a,b) in enumerate((a,b) for a in range(n) for b in range(a+1,n)):
        pair = case['pairs'][p]
        if pair['state']!=2:
            continue
        r = abs(math.dist(solution['nodes'][a],solution['nodes'][b])-pair['distance'])/max(150,pair['spread'])
        cost += r*r if r<=1.5 else 3*r-2.25
    return cost


def choose_full_fit(case, result):
    full = [s for s in result['solutions'] if s['mask']==0]
    if not full:
        return {'accepted':False,'reason':'no_full_fit'}
    best = min(full,key=lambda s:(full_fit_cost(case,s),s['order']))
    return {'accepted':True,'reason':'full_fit','mask':0,'solution':best}


def choose(result, all_orders):
    baseline = result['baseline']
    full = [s for s in result['solutions'] if s['mask']==0]
    if baseline['ok']:
        if all_orders and shape_disagreement(full)>TOLERANCE:
            return {'accepted':False,'reason':'order_unstable'}
        return {'accepted':True,'reason':'baseline','mask':0,'solution':baseline}
    choices = [s for s in result['solutions'] if s['mask'] and (all_orders or s['order']==0)]
    if not choices:
        return {'accepted':False,'reason':'insufficient'}
    if not all_orders:
        minimum = min(s['mask'].bit_count() for s in choices)
        choices = [s for s in choices if s['mask'].bit_count()==minimum]
    masks = {s['mask'] for s in choices}
    if len(masks)!=1 or shape_disagreement(choices)>TOLERANCE:
        return {'accepted':False,'reason':'ambiguous','candidate_masks':sorted(masks)}
    best = min(choices,key=lambda s:(s['residual_mm'],max(s['uncertainty_mm']),s['order']))
    return {'accepted':True,'reason':'recovered','mask':best['mask'],'solution':best}


def synthetic_cases(seed, layouts):
    rng = random.Random(seed)
    cases = []
    for layout in range(layouts):
        points = [[0,0,0]]
        while len(points)<5:
            p = [rng.randrange(-6000,6001),rng.randrange(-6000,6001),rng.randrange(-300,301)]
            if all(math.dist(p,q)>1500 for q in points):
                points.append(p)
        for n in (4,5):
            truth = [p[:] for p in points[:n]]
            count = n*(n-1)//2
            prefix = f'{seed}/{layout}/nodes{n}'
            def add(label, errors=None, **kw):
                case = make_case(prefix+'/'+label,truth,errors,**kw)
                case['class'] = label.split('/')[0]
                cases.append(case)
                return case
            add('clean')
            noisy = add('noise', {p:rng.randrange(-50,51) for p in range(count)})
            noisy['bad_mask'] = 0
            for p in range(count):
                add(f'single/{p}',{p:3000 if p%2==0 else -1000})
            edges = [(a,b) for a in range(n) for b in range(a+1,n)]
            for p in range(count):
                for q in range(p+1,count):
                    kind = 'adjacent' if set(edges[p]) & set(edges[q]) else 'disjoint'
                    add(f'{kind}/{p}/{q}',{p:3000,q:1000})
            flat = [[p[0],p[1],0] for p in truth]
            scaled = make_case(prefix+'/scale',flat)
            scaled['class']='scale'
            for p in scaled['pairs']: p['distance']=round(p['distance']*1.5)
            scaled['bad_mask']=(1<<count)-1
            cases.append(scaled)
            add('common_bias',{p:1000 for p in range(count)})
            star = add('coherent_node_shift')
            shifted = [p[:] for p in truth];shifted[1][0]+=2000
            for index,(a,b) in enumerate(edges):
                if a==1 or b==1:
                    star['pairs'][index]['distance']=round(math.dist(shifted[a],shifted[b]))
                    star['bad_mask'] |= 1<<index
            conflict = add('height_conflict')
            conflict['z'][1]+=3000
            line = make_case(prefix+'/collinear',[[i*2000,0,0] for i in range(n)])
            line['class']='collinear';cases.append(line)
            add('missing',missing_mask=1<<(count-1))
            add('manual_correct',{count-1:3000},manual_mask=1)
            add('manual_conflict',{0:3000},manual_mask=1)
            if n==5:
                add('missing_disjoint',missing_mask=(1<<0)|(1<<9))
                add('missing_adjacent',missing_mask=(1<<8)|(1<<9))
    return cases


def evaluate(cases, results):
    groups = {}
    details = []
    for case, result in zip(cases,results):
        base = result['baseline']
        decisions = {'baseline':{'accepted':base['ok'],'reason':'baseline','mask':0,'solution':base},
                     'single_order':choose(result,False),'all_orders':choose(result,True),
                     'guard_only':choose(result,True) if base['ok'] else {'accepted':False,'reason':'baseline_refusal'}}
        scaled = residual_scaled_uncertainty(case,base) if base['ok'] else None
        decisions['residual_scale'] = {'accepted':base['ok'] and scaled is not None and max(scaled)<=750,
            'reason':'relative_sigma_proxy','mask':0,'solution':base}
        decisions['full_fit'] = choose_full_fit(case,result)
        row = {'name':case['name'],'class':case['class'],'bad_mask':case['bad_mask'],
            'input':case,'decisions':{},'full_shape_disagreement_mm':shape_disagreement([s for s in result['solutions'] if s['mask']==0])}
        for policy,decision in decisions.items():
            d = {k:v for k,v in decision.items() if k!='solution'}
            if decision['accepted']:
                d.update(node_error(decision['solution'],case['truth']))
                d['false_exclusions'] = (decision['mask'] & ~case['bad_mask']).bit_count()
                d['residual_mm'] = decision['solution']['residual_mm']
                d['node_positions_mm'] = decision['solution']['nodes']
            row['decisions'][policy] = d
            for group in ('all',f"nodes{len(case['truth'])}",case['class']):
                bucket = groups.setdefault(group,{}).setdefault(policy,{'n':0,'accepted':0,'false_gt750':0,'false_gt1m':0,
                    'false_gt2m':0,'partial':0,'recovered':0,'false_exclusions':0,'ambiguous':0,'max_node_errors':[],'node_errors':[]})
                bucket['n']+=1
                if decision['accepted']:
                    bucket['accepted']+=1
                    bucket['false_gt750']+=d['max_mm']>750
                    bucket['false_gt1m']+=d['max_mm']>1000
                    bucket['false_gt2m']+=d['max_mm']>2000
                    bucket['partial']+=bool(decision['mask']) or any(p['state']!=2 for p in case['pairs'])
                    bucket['recovered']+=decision['reason']=='recovered'
                    bucket['false_exclusions']+=d['false_exclusions']
                    bucket['max_node_errors'].append(d['max_mm'])
                    bucket['node_errors'].extend(d['nodes_mm'][1:])
                else:
                    bucket['ambiguous']+=decision['reason'] in ('ambiguous','order_unstable')
        details.append(row)
    for group in groups.values():
        for bucket in group.values():
            bucket['coverage']=bucket['accepted']/bucket['n']
            for key in ('max_node_errors','node_errors'):
                errors=bucket.pop(key)
                bucket[key+'_p50_mm']=float(np.percentile(errors,50)) if errors else None
                bucket[key+'_p95_mm']=float(np.percentile(errors,95)) if errors else None
    return groups,details


def main():
    BUILD.mkdir(parents=True,exist_ok=True)
    report, all_details = {},[]
    for split,seed,layouts in [('development',11042026,3),('validation',41022026,8),('confirmation',8112026,16)]:
        if split in ('validation','confirmation'):
            files=[Path(__file__),ROOT/'tests/host/ble6_geometry_orders.c',ROOT/'components/nrf_geometry_solver/nrf_geometry_solver.c']
            (BUILD/f'method-before-{split}.json').write_text(json.dumps({str(p.relative_to(ROOT)):
                hashlib.sha256(p.read_bytes()).hexdigest() for p in files},indent=2))
        cases=synthetic_cases(seed,layouts)
        started=time.perf_counter()
        solutions=native_solutions(cases,BUILD/split)
        groups,details=evaluate(cases,solutions)
        report[split]={'seed':seed,'layouts':layouts,'cases':len(cases),'wall_seconds':time.perf_counter()-started,'groups':groups}
        all_details.extend({'split':split,**r} for r in details)
        print(split,len(cases),json.dumps(groups['all']),flush=True)
    (BUILD/'report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    (BUILD/'cases.json').write_text(json.dumps(all_details,indent=2),encoding='utf-8')


if __name__=='__main__':
    main()
