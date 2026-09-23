import { useEffect, useState } from 'react';
import { fetchLocalizationRuns, sendControl } from '../../api/deviceApi';
import type {
  LocalizationPickRecord,
  LocalizationRun,
  LocalizationSessionCsPairs,
  LocalizationSessionGeometry,
} from '../../api/deviceApi';

interface Props {
  objectId: string;
  sessionId: string;
  busy: boolean;
  onClose: () => void;
}

interface FormState {
  propagationSpeedMps: string;
  propagationSpeedUncertaintyMps: string;
  propagationModelName: string;
  timingUncertaintyUs: string;
  noiseWindowSamples: string;
  staWindowSamples: string;
  ltaWindowSamples: string;
  staLtaTriggerRatio: string;
  refineLookbackSamples: string;
  refineLookaheadSamples: string;
  refineAmplitudeFraction: string;
  snrSignalWindowSamples: string;
  minimumSnr: string;
  semblanceWindowHalfSpanSamples: string;
  maxSourceDepthM: string;
}

/*
 * MAX-168 (project rule MAX-132): every field below starts blank rather than
 * pre-filled with a plausible-looking number. Propagation speed, timing
 * uncertainty and every pick-detection parameter must come from this
 * object's own measured/reviewed values -- there is no site-wide default
 * this form could offer without silently guessing on the operator's behalf.
 */
const EMPTY_FORM: FormState = {
  propagationSpeedMps: '',
  propagationSpeedUncertaintyMps: '',
  propagationModelName: '',
  timingUncertaintyUs: '',
  noiseWindowSamples: '',
  staWindowSamples: '',
  ltaWindowSamples: '',
  staLtaTriggerRatio: '',
  refineLookbackSamples: '',
  refineLookaheadSamples: '',
  refineAmplitudeFraction: '',
  snrSignalWindowSamples: '',
  minimumSnr: '',
  semblanceWindowHalfSpanSamples: '',
  maxSourceDepthM: '',
};

function positiveNumber(text: string): number | null {
  const value = Number(text);
  return text.trim().length > 0 && Number.isFinite(value) && value > 0 ? value : null;
}

function nonNegativeNumber(text: string): number | null {
  const value = Number(text);
  return text.trim().length > 0 && Number.isFinite(value) && value >= 0 ? value : null;
}

/*
 * MAX-210: STA is the trailing sub-window of LTA, so STA/LTA can never exceed
 * LTA/STA. A threshold at or above that bound silently never triggers.
 */
export function staLtaThresholdIssue(
  staWindowSamples: number | null,
  ltaWindowSamples: number | null,
  triggerRatio: number | null,
): string | null {
  if (staWindowSamples === null || ltaWindowSamples === null || triggerRatio === null) return null;
  if (ltaWindowSamples <= staWindowSamples) return 'LTA window must be longer than the STA window.';
  const maximum = ltaWindowSamples / staWindowSamples;
  if (triggerRatio >= maximum) {
    return `STA/LTA can never reach ${triggerRatio}: with these windows the ratio is at most ${maximum.toFixed(2)}.`;
  }
  return null;
}

function positiveInteger(text: string): number | null {
  const value = Number(text);
  return text.trim().length > 0 && Number.isInteger(value) && value > 0 ? value : null;
}

function formatPosition(run: LocalizationRun): string {
  if (run.source_position === undefined) return '—';
  const { x_m, y_m, z_m } = run.source_position;
  return `${x_m.toFixed(2)}, ${y_m.toFixed(2)}, ${z_m.toFixed(2)} m`;
}

function formatMm(value: number | undefined): string {
  return value === undefined ? '—' : `${(value / 1000).toFixed(2)} m`;
}

function nodeLabel(node: number): string {
  return node === 0 ? 'MAIN' : `Pod ${node}`;
}

function pickStatusLabel(status: LocalizationPickRecord['channels'][number]['status']): string {
  switch (status) {
    case 'accepted': return 'accepted';
    case 'rejected_no_trigger': return 'no trigger';
    case 'rejected_no_refinement': return 'no refinement';
    case 'rejected_low_snr': return 'low SNR';
    case 'rejected_inconsistent': return 'inconsistent with other channels';
    default: return status;
  }
}

function latestPickRecord(picks: LocalizationPickRecord[]): LocalizationPickRecord | null {
  return picks.reduce<LocalizationPickRecord | null>(
    (latest, record) => (latest === null || record.computed_at_ms > latest.computed_at_ms ? record : latest),
    null,
  );
}

export default function LocalizationCalculatePanel({ objectId, sessionId, busy, onClose }: Props) {
  const [form, setForm] = useState<FormState>(EMPTY_FORM);
  const [error, setError] = useState<string | null>(null);
  const [running, setRunning] = useState(false);
  const [runs, setRuns] = useState<LocalizationRun[] | null>(null);
  const [geometry, setGeometry] = useState<LocalizationSessionGeometry | null>(null);
  const [csPairs, setCsPairs] = useState<LocalizationSessionCsPairs | null>(null);
  const [picks, setPicks] = useState<LocalizationPickRecord[] | null>(null);
  const [geometryError, setGeometryError] = useState<string | null>(null);

  useEffect(() => {
    let cancelled = false;
    setGeometry(null);
    setCsPairs(null);
    setPicks(null);
    setGeometryError(null);
    fetchLocalizationRuns(objectId, sessionId)
      .then((listing) => {
        if (cancelled) return;
        setGeometry(listing.geometry);
        setCsPairs(listing.cs_pairs);
        setPicks(listing.picks);
        setRuns(listing.runs.length > 0 ? listing.runs : null);
      })
      .catch((reason: unknown) => {
        if (cancelled) return;
        setGeometryError(reason instanceof Error ? reason.message : String(reason));
      });
    return () => { cancelled = true; };
  }, [objectId, sessionId]);

  const set = (key: keyof FormState) => (event: React.ChangeEvent<HTMLInputElement>) => {
    setForm((previous) => ({ ...previous, [key]: event.target.value }));
  };

  const parsed = {
    propagationSpeedMps: positiveNumber(form.propagationSpeedMps),
    propagationSpeedUncertaintyMps: nonNegativeNumber(form.propagationSpeedUncertaintyMps),
    timingUncertaintyUs: positiveNumber(form.timingUncertaintyUs),
    noiseWindowSamples: positiveInteger(form.noiseWindowSamples),
    staWindowSamples: positiveInteger(form.staWindowSamples),
    ltaWindowSamples: positiveInteger(form.ltaWindowSamples),
    staLtaTriggerRatio: positiveNumber(form.staLtaTriggerRatio),
    refineLookbackSamples: positiveInteger(form.refineLookbackSamples),
    refineLookaheadSamples: positiveInteger(form.refineLookaheadSamples),
    refineAmplitudeFraction: positiveNumber(form.refineAmplitudeFraction),
    snrSignalWindowSamples: positiveInteger(form.snrSignalWindowSamples),
    minimumSnr: nonNegativeNumber(form.minimumSnr),
    semblanceWindowHalfSpanSamples: positiveInteger(form.semblanceWindowHalfSpanSamples),
  };
  /* Optional (MAX-211): blank selects the geometry-derived default depth. */
  const maxSourceDepthM = form.maxSourceDepthM.trim().length === 0 ? undefined : positiveNumber(form.maxSourceDepthM);
  const staLtaIssue = staLtaThresholdIssue(parsed.staWindowSamples, parsed.ltaWindowSamples, parsed.staLtaTriggerRatio);
  const formValid =
    Object.values(parsed).every((value) => value !== null) &&
    maxSourceDepthM !== null &&
    staLtaIssue === null;

  const runCalculate = (): void => {
    if (!formValid) return;
    setRunning(true);
    setError(null);
    setRuns(null);

    void sendControl('calculate_source', {
      object_id: objectId,
      session_id: sessionId,
      propagation_speed_mps: parsed.propagationSpeedMps,
      propagation_speed_uncertainty_mps: parsed.propagationSpeedUncertaintyMps,
      propagation_model_name: form.propagationModelName.trim(),
      timing_uncertainty_us: parsed.timingUncertaintyUs,
      noise_window_samples: parsed.noiseWindowSamples,
      sta_window_samples: parsed.staWindowSamples,
      lta_window_samples: parsed.ltaWindowSamples,
      sta_lta_trigger_ratio: parsed.staLtaTriggerRatio,
      refine_lookback_samples: parsed.refineLookbackSamples,
      refine_lookahead_samples: parsed.refineLookaheadSamples,
      refine_amplitude_fraction: parsed.refineAmplitudeFraction,
      snr_signal_window_samples: parsed.snrSignalWindowSamples,
      minimum_snr: parsed.minimumSnr,
      semblance_window_half_span_samples: parsed.semblanceWindowHalfSpanSamples,
      ...(maxSourceDepthM !== undefined ? { max_source_depth_m: maxSourceDepthM } : {}),
    })
      .then((result) => {
        if (!result.ok) throw new Error(result.error ?? 'calculate_source failed');
        return fetchLocalizationRuns(objectId, sessionId);
      })
      .then((listing) => {
        setRuns(listing.runs);
        setPicks(listing.picks);
      })
      .catch((reason: unknown) => setError(reason instanceof Error ? reason.message : String(reason)))
      .finally(() => setRunning(false));
  };

  return (
    <article className="console-card">
      <div className="card-heading">
        <div><span className="eyebrow">SESSION {sessionId}</span><h2>Calculate source</h2></div>
        <button disabled={busy || running} onClick={onClose}>Close</button>
      </div>
      <p>
        Runs every localization backend (TDOA, traveltime inversion, semblance, energy baseline, joint fusion)
        against this session's frozen geometry and raw samples. Every value below must come from this
        object's own measured/reviewed calibration -- nothing here is a guessed default.
      </p>

      <h3>Sensor geometry (frozen for this session)</h3>
      {geometryError !== null && <div className="console-alert" role="alert">{geometryError}</div>}
      {geometry === null && geometryError === null && <p>Loading sensor geometry…</p>}
      {geometry !== null && !geometry.available && (
        <p>No frozen geometry was recorded for this session (recorded before per-session geometry freezing, or never solved).</p>
      )}
      {geometry !== null && geometry.available && (
        <dl className="console-dl">
          {geometry.pods.map((pod) => (
            <div key={pod.pod}>
              <dt>Pod {pod.pod}</dt>
              <dd>
                {pod.computed
                  ? `${formatMm(pod.x_mm)}, ${formatMm(pod.y_mm)}, ${formatMm(pod.z_mm)} · ${pod.leveled ? 'level' : 'not level'}` +
                    (pod.residual_mm !== undefined ? ` · CS residual ${formatMm(pod.residual_mm)}` : '')
                  : 'not participating'}
              </dd>
            </div>
          ))}
        </dl>
      )}

      {csPairs !== null && csPairs.available && csPairs.pairs.length > 0 && (
        <details className="settings-details">
          <summary>Channel Sounding pair evidence ({csPairs.pairs.length})</summary>
          <div className="session-list">
            {csPairs.pairs.map((pair) => (
              <div className="session-row" key={`${pair.node_a}-${pair.node_b}`}>
                <span>
                  <strong>{nodeLabel(pair.node_a)} ↔ {nodeLabel(pair.node_b)}</strong>
                  <small>{pair.valid ? 'valid' : 'invalid'} · {pair.attempts} attempt{pair.attempts === 1 ? '' : 's'}</small>
                </span>
                <span>
                  <strong>{(pair.distance_mm / 1000).toFixed(2)} m</strong>
                  <small>confidence {(pair.confidence / 1000).toFixed(2)} · {pair.valid_samples} samples{pair.spread_mm !== undefined && pair.valid_samples > 1 ? ` · spread ${formatMm(pair.spread_mm)}` : ''}{pair.rejected_samples ? ` · ${pair.rejected_samples} outlier${pair.rejected_samples === 1 ? '' : 's'} dropped` : ''}</small>
                </span>
              </div>
            ))}
          </div>
        </details>
      )}

      {picks !== null && picks.length > 0 && (() => {
        const latest = latestPickRecord(picks);
        if (latest === null) return null;
        return (
          <details className="settings-details">
            <summary>Arrival pick evidence (latest calculate, {picks.length} on record)</summary>
            <div className="session-list">
              {latest.channels.map((channel) => (
                <div className="session-row" key={channel.channel}>
                  <span>
                    <strong>Channel {channel.channel}</strong>
                    <small>{channel.participating ? pickStatusLabel(channel.status) : 'not participating'}</small>
                  </span>
                  <span>
                    <strong>{channel.status === 'accepted' ? `${channel.arrival_time_s.toFixed(3)} s` : '—'}</strong>
                    <small>SNR {channel.snr.toFixed(1)}</small>
                  </span>
                </div>
              ))}
            </div>
          </details>
        );
      })()}

      <div className="cable-grid">
        <label>
          <span>Propagation speed (m/s)</span>
          <input type="number" min="0" step="any" value={form.propagationSpeedMps} onChange={set('propagationSpeedMps')} />
        </label>
        <label>
          <span>Speed uncertainty (m/s)</span>
          <input type="number" min="0" step="any" value={form.propagationSpeedUncertaintyMps} onChange={set('propagationSpeedUncertaintyMps')} />
        </label>
        <label>
          <span>Propagation model name (optional)</span>
          <input type="text" maxLength={31} value={form.propagationModelName} onChange={set('propagationModelName')} />
        </label>
        <label>
          <span>Timing uncertainty (µs)</span>
          <input type="number" min="0" step="any" value={form.timingUncertaintyUs} onChange={set('timingUncertaintyUs')} />
        </label>
      </div>

      <details className="settings-details">
        <summary>Pick-detection config (required)</summary>
        <div className="cable-grid">
          <label>
            <span>Noise window (samples)</span>
            <input type="number" min="1" step="1" value={form.noiseWindowSamples} onChange={set('noiseWindowSamples')} />
          </label>
          <label>
            <span>STA window (samples)</span>
            <input type="number" min="1" step="1" value={form.staWindowSamples} onChange={set('staWindowSamples')} />
          </label>
          <label>
            <span>LTA window (samples)</span>
            <input type="number" min="1" step="1" value={form.ltaWindowSamples} onChange={set('ltaWindowSamples')} />
          </label>
          <label>
            <span>STA/LTA trigger ratio</span>
            <input type="number" min="0" step="any" value={form.staLtaTriggerRatio} onChange={set('staLtaTriggerRatio')} />
          </label>
          <label>
            <span>Refine lookback (samples)</span>
            <input type="number" min="1" step="1" value={form.refineLookbackSamples} onChange={set('refineLookbackSamples')} />
          </label>
          <label>
            <span>Refine lookahead (samples)</span>
            <input type="number" min="1" step="1" value={form.refineLookaheadSamples} onChange={set('refineLookaheadSamples')} />
          </label>
          <label>
            <span>Refine amplitude fraction</span>
            <input type="number" min="0" max="1" step="any" value={form.refineAmplitudeFraction} onChange={set('refineAmplitudeFraction')} />
          </label>
          <label>
            <span>SNR signal window (samples)</span>
            <input type="number" min="1" step="1" value={form.snrSignalWindowSamples} onChange={set('snrSignalWindowSamples')} />
          </label>
          <label title="SNR here is the peak |amplitude| after the pick divided by the RMS of the noise window at the start of the recording. Pure noise already scores above 1 (its own peaks exceed its RMS), so choose the threshold from this site's measured recordings.">
            <span>Minimum SNR (peak / noise RMS)</span>
            <input type="number" min="0" step="any" value={form.minimumSnr} onChange={set('minimumSnr')} />
          </label>
          <label>
            <span>Semblance window half-span (samples)</span>
            <input type="number" min="1" step="1" value={form.semblanceWindowHalfSpanSamples} onChange={set('semblanceWindowHalfSpanSamples')} />
          </label>
          <label title="How far below the lowest geophone a 3D search may place the source. Blank = half the array diagonal (at least 1 m). The search never goes above the highest geophone.">
            <span>Max source depth (m, optional)</span>
            <input type="number" min="0" step="any" value={form.maxSourceDepthM} onChange={set('maxSourceDepthM')} />
          </label>
        </div>
        {staLtaIssue !== null && <div className="console-alert" role="alert">{staLtaIssue}</div>}
        {maxSourceDepthM === null && <div className="console-alert" role="alert">Max source depth must be a positive number of meters, or blank.</div>}
      </details>

      {error !== null && <div className="console-alert" role="alert">{error}</div>}

      <div className="console-actions">
        <button disabled={busy || running || !formValid} onClick={runCalculate}>
          {running ? 'Calculating…' : 'Calculate source'}
        </button>
      </div>

      {runs !== null && (
        <>
        <h3>Localization runs</h3>
        <div className="session-list">
          {runs.length === 0 ? (
            <p>No localization runs persisted for this session.</p>
          ) : runs.map((run) => (
            <div className="session-row" key={`${run.method_name}-${run.computed_at_ms}`}>
              <span>
                <strong>{run.method_name}</strong>
                <small>{run.kind === 'joint_fusion' ? 'joint fusion' : run.dimensionality}</small>
              </span>
              <span>
                <strong>{run.solved ? formatPosition(run) : 'unsolved'}</strong>
                <small>
                  {run.solved
                    ? `confidence ${run.confidence_score.toFixed(2)}` +
                      (run.degrees_of_freedom === 0 ? ' · unverified (no redundant picks)' : '') +
                      (run.method_name === 'energy_baseline' ? ' · diagnostic only, not fused' : '')
                    : run.rejection_reason}
                </small>
              </span>
            </div>
          ))}
        </div>
        </>
      )}
    </article>
  );
}
