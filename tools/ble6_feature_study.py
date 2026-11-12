"""Legacy with_peak/raw-distance feature comparison; no fitted cutoff or policy change."""
from __future__ import annotations
import csv
import hashlib
import itertools
import json
import math
import statistics
import bisect
from pathlib import Path
import ble6_research_replay as replay
from ble6_series_benchmark import metrics

BUILD=replay.BUILD/'feature-study'
CUTS=(.3,.75,1.,2.5)  # Exploratory reporting bins, never a firmware configuration.


def finite(value):
    return float(value) if value is not None and math.isfinite(value) else None


def features(case,estimate,quality):
    counts=case['counts']
    if len(counts)!=75:raise ValueError('Expected75 channel slots')
    usable=[n>0 for n in counts]
    runs=[len(list(g)) for present,g in itertools.groupby(usable) if not present]
    lead=next((i for i,v in enumerate(usable) if v),75)
    tail=next((i for i,v in enumerate(reversed(usable)) if v),75)
    raw=case.get('raw_steps',b'')
    if len(raw)%11:raise ValueError('Raw paired tones require11 bytes per step')
    bad=sum((raw[i+5]&15)!=0 or (raw[i+10]&15)!=0 for i in range(0,len(raw),11))
    best,phase=finite(estimate[1]),finite(estimate[2])
    rtt=finite(case.get('rtt'))
    rtt_valid=rtt is not None and rtt>0
    diagnostic=case.get('diagnostics',{})
    def rssi_median(name):
        values=[v for v in diagnostic.get(name) or [] if v is not None and math.isfinite(v) and v!=127]
        return statistics.median(values) if values else None
    tqi=diagnostic.get('tqi') or []
    return {'distance_m':best,'phase_m':phase,'confidence':int(quality[2]),
        'plain_confidence':int(quality[1]),'why':int(quality[3]),'peak_to_floor':finite(quality[0]),
        'method_gap_m':abs(best-phase) if best is not None and phase is not None else None,
        'rtt_valid':rtt_valid,'rtt_gap_m':abs(best-rtt) if best is not None and rtt_valid else None,
        'usable_channels':sum(usable),'planned_channels':None,
        'longest_absence':max(runs,default=0),'leading_absence':lead,'trailing_absence':tail,
        'raw_bad_tone_fraction':bad/(len(raw)//11) if raw else None,
        'local_tone_rssi_median_dbm':rssi_median('tone_RSSI'),
        'remote_tone_rssi_median_dbm':rssi_median('remote_tone_RSSI'),
        'recorded_median_rssi_dbm':finite(diagnostic.get('median_RSSI')),
        'non_high_tqi_fraction':sum(q!='HIGH' for q in tqi)/len(tqi) if tqi else None,
        'author_de_quality':diagnostic.get('de_quality'),
        **{'author_'+name:finite(diagnostic.get(name)) for name in ('local_rpl','remote_rpl','local_tx','remote_tx','nak','crc','rx_to')}}


def accept(feature,limit=None):
    best=feature['distance_m']
    if best is None or not math.isfinite(best) or best<0 or feature['confidence']<500:return False
    gap=feature.get('method_gap_m')
    return limit is None or (gap is not None and math.isfinite(gap) and gap<=limit)


def compare(rows):
    result={}
    for name,limit in [('all_estimated',None),('plain',None),('legacy_peak',None)]+[(f'gap_{n:g}m',n) for n in CUTS]:
        errors=[]
        for row in rows:
            f=row['feature'];truth=row['truth_m']
            if truth is None or not math.isfinite(truth):continue
            if name=='all_estimated':
                if f['distance_m'] is None or f['distance_m']<0:continue
            elif name=='plain':
                if not accept({**f,'confidence':f['plain_confidence']}):continue
            elif not accept(f,limit):continue
            errors.append(f['distance_m']-truth)
        result[name]=metrics(errors)|{'coverage':len(errors)/len(rows) if rows else 0,
                                     'good_1m':sum(abs(e)<=1 for e in errors)}
    return result


def associations(rows):
    result={}
    for name in ('usable_channels','longest_absence','peak_to_floor','method_gap_m','rtt_gap_m',
                 'local_tone_rssi_median_dbm','remote_tone_rssi_median_dbm','non_high_tqi_fraction',
                 'author_crc','author_nak','author_rx_to'):
        values=[r['feature'][name] for r in rows if r['feature'][name] is not None]
        if not values:
            result[name]={'missing':len(rows)};continue
        edges=[replay.percentile(values,q) for q in (.25,.5,.75)]
        buckets=[[] for _ in range(4)];missing=0
        for row in rows:
            value=row['feature'][name]
            if value is None:missing+=1;continue
            buckets[bisect.bisect_right(edges,value)].append(row)
        result[name]={'descriptive_quartiles':edges,'missing':missing,
            'bins':[{'inputs':len(b),'legacy_peak':compare(b)['legacy_peak']} for b in buckets]}
    labels=sorted({r['feature']['author_de_quality'] for r in rows if r['feature']['author_de_quality'] is not None})
    result['author_de_quality']={'missing':sum(r['feature']['author_de_quality'] is None for r in rows),
        'groups':{label:compare([r for r in rows if r['feature']['author_de_quality']==label])['legacy_peak'] for label in labels}}
    return result


def main():
    cases=list(replay.ocm_cases())+list(replay.wieme_cases())
    for name in ('owl-indoor.parquet','owl-outdoor.parquet'):
        cases.extend(itertools.islice(replay.owl_cases(replay.OWL/name,stride=1,include_diagnostics=True),5000))
    values,source=replay.build_and_replay(cases,workdir=BUILD/'iq-replay')
    quality=replay.confidence_extra(BUILD/'iq-replay')
    rows=[];groups={};examples={}
    for case,v,q in zip(cases,values,quality,strict=True):
        row={k:case.get(k) for k in ('dataset','file','row','env','setup','run_idx','angle','truth_m')}
        row['feature']=features(case,v,q)
        # Whole source/session groups, never adjacent rows split between report slices.
        key='/'.join(str(row[k]) for k in ('dataset','file','env','setup','run_idx'))
        row['slice']='reserved' if int(hashlib.sha256(('b2-features/v1/'+key).encode()).hexdigest()[:8],16)%5==0 else 'other'
        rows.append(row)
        condition='NLOS' if 'NLOS' in row['file'].split('/') else 'LOS' if 'LOS' in row['file'].split('/') else 'unlabelled'
        for group in (row['dataset'],row['dataset']+'/'+condition,row['dataset']+'/'+row['slice']):
            groups.setdefault(group,[]).append(row)
        f=row['feature']
        if accept(f) and row['truth_m'] is not None:
            error=abs(f['distance_m']-row['truth_m'])
            kind='wrong_despite_agreement' if error>2 and accept(f,.3) else 'good_despite_disagreement' if error<=1 and not accept(f,2.5) else None
            if kind and len(examples.setdefault(row['dataset']+'/'+kind,[]))<3:
                examples[row['dataset']+'/'+kind].append(row)
    result={'inputs':len(rows),'groups':{k:compare(v) for k,v in groups.items()},'examples':examples,
        'feature_error_associations':{k:associations(groups[k]) for k in ('OCMTEC','Wieme','OWL')},
        'sdk_sha256':replay.sha256(source),'offset_m':0,'diagnostic_cuts_m':CUTS,
        'confidence_policy':'legacy with_peak (extra.bin)','distance_basis':'raw, no calibration',
        'native_artifacts_sha256':{n:replay.sha256(BUILD/'iq-replay'/n) for n in ('input.bin','output.bin','extra.bin','collector-provenance.json')},
        'limitations':['exploratory bins; no rule selected or fitted',
            'method gap compares chosen best with phase_slope; equal outputs may be phase fallback, not independent confirmation',
            'datasets previously inspected; reserved source/session slice is not a new external holdout',
            'single-link procedures, not synchronous geometry matrices',
            'host FFT and production collector; confidence is legacy with_peak, not current firmware; no physical radio validation',
            'planned channel map and per-packet AA quality unavailable; OWL author counters are not AA validity',
            'absence on75-slot grid is not known planned-channel loss',
            'OCMTEC includes systematic bias; no offset subtracted',
            'OWL first5000 usable source rows per file; no stride substitution']}
    BUILD.mkdir(parents=True,exist_ok=True)
    with (BUILD/'features.csv').open('w',encoding='utf-8',newline='') as stream:
        flat=[{k:v for k,v in row.items() if k!='feature'}|row['feature'] for row in rows]
        writer=csv.DictWriter(stream,fieldnames=list(flat[0]));writer.writeheader();writer.writerows(flat)
    (BUILD/'report.json').write_text(json.dumps(result,indent=2,allow_nan=False),encoding='utf-8')
    print(json.dumps({'inputs':len(rows),'groups':{k:result['groups'][k] for k in ('OCMTEC','Wieme','OWL')},
                      'examples':examples},indent=2,allow_nan=False),flush=True)


if __name__=='__main__':main()
