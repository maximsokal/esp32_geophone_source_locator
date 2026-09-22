import { useState } from 'react';
import { fetchLocalizationRuns, sendControl } from '../../api/deviceApi';
import type { LocalizationRun } from '../../api/deviceApi';

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
};

function positiveNumber(text: string): number | null {
  const value = Number(text);
  return text.trim().length > 0 && Number.isFinite(value) && value > 0 ? value : null;
}

function nonNegativeNumber(text: string): number | null {
  const value = Number(text);
  return text.trim().length > 0 && Number.isFinite(value) && value >= 0 ? value : null;
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

export default function LocalizationCalculatePanel({ objectId, sessionId, busy, onClose }: Props) {
  const [form, setForm] = useState<FormState>(EMPTY_FORM);
  const [error, setError] = useState<string | null>(null);
  const [running, setRunning] = useState(false);
  const [runs, setRuns] = useState<LocalizationRun[] | null>(null);

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
  const formValid = Object.values(parsed).every((value) => value !== null);

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
    })
      .then((result) => {
        if (!result.ok) throw new Error(result.error ?? 'calculate_source failed');
        return fetchLocalizationRuns(objectId, sessionId);
      })
      .then((listing) => setRuns(listing.runs))
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
          <label>
            <span>Minimum SNR</span>
            <input type="number" min="0" step="any" value={form.minimumSnr} onChange={set('minimumSnr')} />
          </label>
          <label>
            <span>Semblance window half-span (samples)</span>
            <input type="number" min="1" step="1" value={form.semblanceWindowHalfSpanSamples} onChange={set('semblanceWindowHalfSpanSamples')} />
          </label>
        </div>
      </details>

      {error !== null && <div className="console-alert" role="alert">{error}</div>}

      <div className="console-actions">
        <button disabled={busy || running || !formValid} onClick={runCalculate}>
          {running ? 'Calculating…' : 'Calculate source'}
        </button>
      </div>

      {runs !== null && (
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
                    ? `confidence ${run.confidence_score.toFixed(2)}`
                    : run.rejection_reason}
                </small>
              </span>
            </div>
          ))}
        </div>
      )}
    </article>
  );
}
