"""Before/after production solver evidence for independent mirror ambiguity."""
import json
import random
from pathlib import Path
import ble6_geometry_consensus as geometry
from ble6_series_benchmark import metrics
from ble6_research_replay import sha256

BUILD=geometry.ROOT/'.test-build/ble6-mirror-study'


def cases(seed,count):
    rng=random.Random(seed);result=[]
    for i in range(count):
        a=rng.randrange(3500,5501);b=a+rng.randrange(3500,5501)
        x=rng.randrange(1000,a);y=rng.randrange(a,b-500)
        h=rng.randrange(2500,4501);k=rng.randrange(2500,4501)
        z=[0,0,0,rng.randrange(-300,301),rng.randrange(-300,301)]
        points=[[0,0,0],[a,0,0],[b,0,0],[x,h,z[3]],[y,-k,z[4]]]
        same=[p[:] for p in points];same[4][1]*=-1
        square=[[0,0,0],[a,0,0],[0,b,0],[round(.8*a),round(.8*b),z[3]],
                [round(.3*a),round(.35*b),z[4]]]
        for label,xy,missing in [('ambiguous_opposite',points,1<<9),('ambiguous_same',same,1<<9),
                                 ('complete_link',points,0),('partial_identifiable',square,1<<9)]:
            c=geometry.make_case(f'{seed}/{i}/{label}',xy,missing_mask=missing)
            c['class']=label;result.append(c)
        assert all(result[-4]['pairs'][p]['distance']==result[-3]['pairs'][p]['distance'] for p in range(9))
    return result


def summarize(inputs,solutions):
    groups={}
    for c,s in zip(inputs,solutions,strict=True):
        for key in ('all',c['class']):
            bucket=groups.setdefault(key,{'matrices':0,'accepted':0,'accepted_orders':0,'wrong_gt750':0,'errors':[]})
            bucket['matrices']+=1
            bucket['accepted_orders']+=sum(row['mask']==0 for row in s['solutions'])
            if s['baseline']['ok']:
                error=geometry.node_error(s['baseline'],c['truth'])['max_mm']
                bucket['accepted']+=1;bucket['wrong_gt750']+=error>750;bucket['errors'].append(error/1000)
    for bucket in groups.values():
        bucket['error_metrics_m']=metrics(bucket.pop('errors'))
        bucket['error_metrics_m'].pop('bias_m')  # Max-node error is unsigned, so it has no signed bias.
    return groups


def main():
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before',required=True,help='Repository-relative copy of pre-fix production solver')
    args=parser.parse_args()
    source=geometry.ROOT/args.before
    if not source.is_file():parser.error('Missing pre-fix source')
    fresh=[]
    for seed in range(110420261,110420267):fresh.extend(cases(seed,16))
    confirmation=[]
    for seed in (110420267,110420268,110420269):confirmation.extend(cases(seed,16))
    historical=[]
    for seed,n in ((11042026,3),(41022026,8),(8112026,16)):
        historical.extend(geometry.synthetic_cases(seed,n))
    report={'before_sha256':sha256(source),'after_sha256':sha256(geometry.ROOT/'components/nrf_geometry_solver/nrf_geometry_solver.c'),
            'development_seeds':list(range(110420261,110420267)),
            'confirmation_seeds':[110420267,110420268,110420269],'splits':{}}
    for name,inputs in [('development_mirrors',fresh),('confirmation_mirrors',confirmation),('historical_stress',historical)]:
        print('RUN',name,len(inputs),flush=True)
        before=geometry.native_solutions(inputs,BUILD/name/'before',args.before)
        after=geometry.native_solutions(inputs,BUILD/name/'after')
        changes=[]
        for c,a,b in zip(inputs,before,after,strict=True):
            if a['baseline']['ok']!=b['baseline']['ok'] or a['baseline']['nodes']!=b['baseline']['nodes']:
                changes.append({'name':c['name'],'class':c['class'],'before_ok':a['baseline']['ok'],
                    'after_ok':b['baseline']['ok'],
                    'before_error_mm':geometry.node_error(a['baseline'],c['truth'])['max_mm'] if a['baseline']['ok'] else None,
                    'after_error_mm':geometry.node_error(b['baseline'],c['truth'])['max_mm'] if b['baseline']['ok'] else None})
        report['splits'][name]={'before':summarize(inputs,before),'after':summarize(inputs,after),'changes':changes}
        print(json.dumps(report['splits'][name]['after']),flush=True)
    report['limitations']=['synthetic known-coordinate matrices; no RF distance correction',
        'historical seeds were previously inspected; fresh mirror classes defined before these runs',
        'truth XY is used only for evaluation, never passed into native solver',
        'same/opposite ambiguous layouts deliberately have identical observed edges',
        'a chance-correct ambiguous layout still requires refusal, not guessing']
    BUILD.mkdir(parents=True,exist_ok=True)
    (BUILD/'report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print('REPORT',BUILD/'report.json',flush=True)


if __name__=='__main__':main()
