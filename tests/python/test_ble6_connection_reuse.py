import sys
from pathlib import Path
import pytest

sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
import ble6_connection_reuse as reuse


def trace(attempt=1,queue=20):
    lines=[f'cs: t=0 a={attempt} execute init=0 refl=1 budget=2400ms',
           f'cs: t=20 a={attempt} ev=EXECUTE_WORK_STARTED phase=CONNECTING +{queue}ms']
    for i,name in enumerate(reuse.SETUP_STAGES,1):
        lines.append(f'cs: t={20+i*10} a={attempt} ev={name} phase=CONNECTING +{queue+i*10}ms')
    lines.append(f'cs: t=150 a={attempt} result=SUCCESS measured=130ms cleanup=20ms +150ms')
    return '\n'.join(lines)


def test_reuse_cost_preserves_queue_wait_and_cleanup():
    rows,skipped=reuse.extract_profiles(trace(),'fixture')
    assert skipped==0 and len(rows)==1
    assert rows[0]['cold_ms']==150 and rows[0]['setup_ms']==80 and rows[0]['warm_ms']==70
    assert rows[0]['cleanup_ms']==20 and rows[0]['queue_ms']==20


def test_incomplete_or_contradictory_timing_is_not_a_profile():
    rows,skipped=reuse.extract_profiles(trace().replace('CS_SECURITY_READY','UNKNOWN'),'fixture')
    assert rows==[] and skipped==1
    rows,skipped=reuse.extract_profiles(trace().replace('cleanup=20ms','cleanup=200ms'),'fixture')
    assert rows==[] and skipped==1


def test_reused_attempt_ids_are_not_silently_merged():
    with pytest.raises(ValueError,match='Duplicate execute'):
        reuse.extract_profiles(trace()+'\n'+trace(),'fixture')


def test_profile_quantiles_select_a_whole_observed_attempt():
    rows=[{'cold_ms':i,'warm_ms':i//2,'file':'fixture','attempt':i} for i in range(1,21)]
    selected=reuse.select_profiles(rows)
    assert [p['cold_ms'] for p in selected]==[10,19,20]
    assert all(p in rows for p in selected)


def test_native_reuse_model_preserves_samples_and_needs_relay_release():
    rows=reuse.native_model([{'cold_ms':1400,'warm_ms':300}],reuse.BUILD/'test-native')
    assert len(rows)==90
    five={ (r['scenario'],r['reuse']):r for r in rows if r['nodes']==5 and r['rate']==100000 }
    assert five[0,0]['records']==five[0,1]['records']==180
    assert five[0,0]['handshakes']==180 and five[0,1]['handshakes']==20
    assert five[0,1]['elapsed_ms']<five[0,0]['elapsed_ms']
    assert five[4,0]['elapsed_ms']==five[4,1]['elapsed_ms']
    assert all(r['accepted']==(r['nodes']>=4) for r in rows)
