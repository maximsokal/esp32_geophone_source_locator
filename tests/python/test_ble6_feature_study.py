import sys
import csv
import os
from pathlib import Path
import pytest

sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
import ble6_feature_study as study


def test_features_do_not_use_reference_distance_and_distinguish_missing_rtt():
    case={'counts':[1]*75,'rtt':float('nan'),'truth_m':2,'raw_steps':b''}
    a=study.features(case,(0,4,4.1,0),(20,400,600,5))
    b=study.features({**case,'truth_m':999},(0,4,4.1,0),(20,400,600,5))
    assert a==b and a['method_gap_m']==pytest.approx(.1)
    assert a['rtt_valid'] is False and a['rtt_gap_m'] is None
    assert a['raw_bad_tone_fraction'] is None
    assert study.accept(a,.3)


def test_absent_slots_are_not_claimed_to_be_excluded_planned_channels():
    counts=[0]*10+[1]*60+[0]*5
    f=study.features({'counts':counts,'rtt':8},(0,4,6,0),(20,400,600,5))
    assert (f['usable_channels'],f['longest_absence'],f['leading_absence'],f['trailing_absence'])==(60,10,10,5)
    assert f['planned_channels'] is None and f['rtt_gap_m']==4
    assert not study.accept(f,.75)


def test_bad_reference_cannot_change_candidate_acceptance_or_hide_good_losses():
    rows=[{'feature':{'distance_m':3.,'phase_m':3.1,'method_gap_m':.1,'confidence':600,'plain_confidence':400},'truth_m':1.},
          {'feature':{'distance_m':1.,'phase_m':7.,'method_gap_m':6.,'confidence':600,'plain_confidence':400},'truth_m':1.}]
    report=study.compare(rows)
    assert report['legacy_peak']['good_1m']==1
    assert report['gap_0.3m']['good_1m']==0 and report['gap_0.3m']['coverage']==.5
    assert report['gap_0.3m']['gt_1m']==1
    assert report['plain']['n']==0 and report['all_estimated']['n']==2


def test_malformed_optional_raw_tones_fail_instead_of_silently_changing_denominators():
    with pytest.raises(ValueError,match='11'):
        study.features({'counts':[1]*75,'rtt':0,'raw_steps':b'x'},(0,1,2,0),(20,400,600,5))


def test_optional_rssi_tqi_and_packet_counters_are_preserved_without_becoming_truth():
    case={'counts':[1]*75,'rtt':0,'diagnostics':{'tone_RSSI':[-60,-70,None,127],
        'remote_tone_RSSI':[-80,-70],'tqi':['HIGH','LOW'],'crc':2,'nak':0,'rx_to':1,'local_tx':0,'de_quality':'OK'}}
    f=study.features(case,(0,2,4,0),(20,400,600,5))
    assert f['local_tone_rssi_median_dbm']==-65 and f['remote_tone_rssi_median_dbm']==-75
    assert f['non_high_tqi_fraction']==.5 and f['author_crc']==2 and f['author_nak']==0
    assert f['author_rx_to']==1 and f['author_local_tx']==0
    assert f['author_de_quality']=='OK'


def test_optional_owl_diagnostics_preserve_the_original_iq_and_reference_record():
    import math
    r=study.replay;path=r.OWL/'owl-indoor.parquet'
    if not path.is_file():pytest.skip('Optional OWL dataset unavailable')
    pytest.importorskip('pyarrow')
    normal=next(r.owl_cases(path,stride=1))
    diagnostic=next(r.owl_cases(path,stride=1,include_diagnostics=True))
    for key,value in normal.items():
        if isinstance(value,float) and math.isnan(value):assert math.isnan(diagnostic[key])
        else:assert diagnostic[key]==value
    assert diagnostic['diagnostics']['tone_RSSI'] and diagnostic['diagnostics']['de_quality'] is not None


def test_real_iq_method_agreement_is_not_a_distance_accuracy_certificate():
    r=study.replay
    name='channel_map/NLOS/50_spread.csv'
    path=r.WIEME/'data'/name
    sdk=Path(os.environ.get('BLE6_NCS_ROOT','C:/ncs/v3.4.0/nrf'))
    if not path.is_file() or not (sdk/'subsys/bluetooth/cs_de/cs_de.c').is_file():
        pytest.skip('Optional Wieme dataset or pinned SDK unavailable')
    with (r.WIEME/'mapping.csv').open(encoding='utf-8-sig',newline='') as stream:
        ref=next(row for row in csv.DictReader(stream) if row['filename'].replace(chr(92),'/')==name)
    import math
    truth=math.sqrt(sum((float(ref['initiator_'+a])-float(ref['reflector_'+a]))**2 for a in 'xyz'))/1000
    with path.open(encoding='utf-8') as stream:line=stream.readline()
    iq,counts=r.wieme_row(line)
    case={'counts':counts,'iq':iq,'rtt':math.nan,'raw_steps':r.wieme_raw_steps(line)}
    values,_=r.build_and_replay([case],workdir=study.BUILD/'test-real-agreement')
    quality=r.confidence_extra(study.BUILD/'test-real-agreement')
    f=study.features(case,values[0],quality[0])
    assert f['usable_channels']>=15 and f['distance_m']!=f['phase_m']  # Distinct estimates, no RTT/phase fallback.
    assert study.accept(f,.3) and abs(f['distance_m']-truth)>2
