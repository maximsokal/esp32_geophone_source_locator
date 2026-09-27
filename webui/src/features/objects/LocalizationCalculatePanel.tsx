import { useEffect, useState } from 'react';
import { fetchLocalizationJob, fetchLocalizationRuns, fetchScanInfo, sendControl } from '../../api/deviceApi';
import LocalizationJobReport from './LocalizationJobReport';
import LocalizationOverlay from './LocalizationOverlay';
import {
  jobProgressText,
  waitForLocalizationJob,
  type LocalizationJobStatus,
} from '../../domain/localizationJob';
import { uncertaintyText } from '../../domain/localizationOverlay';
import type {
  LocalizationPickRecord,
  LocalizationGeometryProvenance,
  LocalizationSessionPodGeometry,
  LocalizationRun,
  LocalizationSessionCsPairs,
  LocalizationSessionGeometry,
} from '../../api/deviceApi';
import { useLocale, useT, translate, type I18nLocale, type I18nKey } from '../../i18n';
import { formatDistance, formatNumber } from '../../i18n/format';

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
  timeBudgetS: string;
  sMinSeparationSamples: string;
  sMaxSeparationSamples: string;
  sStaWindowSamples: string;
  sLtaWindowSamples: string;
  sStaLtaTriggerRatio: string;
  sMinimumSnr: string;
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
  timeBudgetS: '',
  sMinSeparationSamples: '',
  sMaxSeparationSamples: '',
  sStaWindowSamples: '',
  sLtaWindowSamples: '',
  sStaLtaTriggerRatio: '',
  sMinimumSnr: '',
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
  locale: I18nLocale = 'en',
): string | null {
  if (staWindowSamples === null || ltaWindowSamples === null || triggerRatio === null) return null;
  if (ltaWindowSamples <= staWindowSamples) return translate(locale, 'calc.sta_lta_longer_error');
  const maximum = ltaWindowSamples / staWindowSamples;
  if (triggerRatio >= maximum) {
    return translate(locale, 'calc.sta_lta_never_error', {
      trigger: formatNumber(locale, triggerRatio),
      maximum: formatNumber(locale, maximum, { minimumFractionDigits: 2, maximumFractionDigits: 2 }),
    });
  }
  return null;
}

function positiveInteger(text: string): number | null {
  const value = Number(text);
  return text.trim().length > 0 && Number.isInteger(value) && value > 0 ? value : null;
}

function nonNegativeInteger(text: string): number | null {
  const value = Number(text);
  return text.trim().length > 0 && Number.isInteger(value) && value >= 0 ? value : null;
}

export interface SWavePickFields {
  minSeparationSamples: string;
  maxSeparationSamples: string;
  staWindowSamples: string;
  ltaWindowSamples: string;
  staLtaTriggerRatio: string;
  minimumSnr: string;
}

export interface SWavePickPayload {
  s_min_separation_samples: number;
  s_max_separation_samples: number;
  s_sta_window_samples: number;
  s_lta_window_samples: number;
  s_sta_lta_trigger_ratio: number;
  s_minimum_snr: number;
}

export type SWavePickSettings =
  | { state: 'off' }
  | { state: 'invalid'; issue: string }
  | { state: 'on'; payload: SWavePickPayload };

/*
 * PSAFUVS needs S-wave arrivals. The S picker is optional, but like the P
 * picker it never runs on guessed windows (MAX-132): all fields blank turns
 * it off (PSAFUVS is then reported as not attempted), all fields valid
 * turns it on, anything in between blocks the calculation.
 */
export function sWavePickSettings(fields: SWavePickFields, locale: I18nLocale = 'en'): SWavePickSettings {
  const texts = Object.values(fields);
  if (texts.every((text) => text.trim().length === 0)) return { state: 'off' };
  if (texts.some((text) => text.trim().length === 0)) {
    return { state: 'invalid', issue: translate(locale, 'calc.s_wave_empty_error') };
  }
  const minSeparation = nonNegativeInteger(fields.minSeparationSamples);
  const maxSeparation = positiveInteger(fields.maxSeparationSamples);
  const sta = positiveInteger(fields.staWindowSamples);
  const lta = positiveInteger(fields.ltaWindowSamples);
  const ratio = positiveNumber(fields.staLtaTriggerRatio);
  const minimumSnr = nonNegativeNumber(fields.minimumSnr);
  if (minSeparation === null || maxSeparation === null || sta === null || lta === null || ratio === null || minimumSnr === null) {
    return { state: 'invalid', issue: translate(locale, 'calc.s_wave_values_error') };
  }
  if (maxSeparation <= minSeparation) {
    return { state: 'invalid', issue: translate(locale, 'calc.s_wave_separation_error') };
  }
  const thresholdIssue = staLtaThresholdIssue(sta, lta, ratio, locale);
  if (thresholdIssue !== null) return { state: 'invalid', issue: translate(locale, 'calc.s_wave_threshold_error', { issue: thresholdIssue }) };
  return {
    state: 'on',
    payload: {
      s_min_separation_samples: minSeparation,
      s_max_separation_samples: maxSeparation,
      s_sta_window_samples: sta,
      s_lta_window_samples: lta,
      s_sta_lta_trigger_ratio: ratio,
      s_minimum_snr: minimumSnr,
    },
  };
}

/** The device's default budget (LOCALIZATION_JOB_DEFAULT_BUDGET_MS). */
export const DEFAULT_TIME_BUDGET_S = 60;

/** Blank: undefined (device default); otherwise 1..600 s, or null when invalid. */
export function timeBudgetSeconds(value: string): number | undefined | null {
  if (value.trim().length === 0) return undefined;
  const parsed = Number(value);
  return Number.isFinite(parsed) && parsed >= 1 && parsed <= 600 ? parsed : null;
}

/** Velocities a method (TDCAS, PSAFUVS, SUEA, PSP, eikonal, Bayesian) estimated for this event. */
export function estimatedVelocityText(run: Pick<LocalizationRun, 'estimated_p_velocity_mps' | 'estimated_s_velocity_mps'>, locale: I18nLocale = 'en'): string {
  const p = run.estimated_p_velocity_mps;
  const s = run.estimated_s_velocity_mps;
  if (p === undefined || !(p > 0)) return '';
  const params = { p: formatNumber(locale, Math.round(p)), s: formatNumber(locale, Math.round(s ?? 0)) };
  return ` · ${s !== undefined && s > 0 ? translate(locale, 'calc.velocity_both', params) : translate(locale, 'calc.velocity_p', params)}`;
}

function formatPosition(run: LocalizationRun, locale: I18nLocale): string {
  if (run.source_position === undefined) return '—';
  const { x_m, y_m, z_m } = run.source_position;
  const options = { minimumFractionDigits: 2, maximumFractionDigits: 2 };
  return `${formatNumber(locale, x_m, options)}, ${formatNumber(locale, y_m, options)}, ${formatNumber(locale, z_m, options)} m`;
}

function formatMm(value: number | undefined, locale: I18nLocale): string {
  return value === undefined ? '—' : formatDistance(locale, value / 1000);
}

function nodeLabel(node: number, locale: I18nLocale): string {
  return node === 0 ? 'MAIN' : translate(locale, 'objects.pod', { number: formatNumber(locale, node) });
}

function presentProvenanceFields(fields: Array<[I18nKey, string | number | undefined]>): Array<{ key: I18nKey; value: string }> {
  return fields.flatMap(([key, value]) => value === undefined ? [] : [{ key, value: String(value) }]);
}

function geometryProvenanceFields(provenance: LocalizationGeometryProvenance, t: (key: I18nKey) => string) {
  return presentProvenanceFields([
    ['calc.provenance.solver', provenance.solver],
    ['calc.provenance.solver_version', provenance.solver_version],
    ['calc.provenance.refined', provenance.refined === undefined ? undefined : t(provenance.refined ? 'report.yes' : 'report.no')],
    ['calc.provenance.refine_iterations', provenance.refine_iterations],
    ['calc.provenance.topology_generation', provenance.topology_generation],
    ['calc.provenance.cs_membership_generation', provenance.cs_membership_generation],
    ['calc.provenance.main_height_reference_generation', provenance.main_height_reference_generation],
    ['calc.provenance.setup_id', provenance.setup_id],
    ['calc.provenance.start_uptime_ms', provenance.start_uptime_ms],
  ]);
}

function podProvenanceFields(pod: LocalizationSessionPodGeometry, t: (key: I18nKey) => string) {
  return presentProvenanceFields([
    ['calc.provenance.source_sample_uptime_ms', pod.source_sample_uptime_ms],
    ['calc.provenance.esp_receipt_uptime_ms', pod.esp_receipt_uptime_ms],
    ['calc.provenance.source_age_ms_at_start', pod.source_age_ms_at_start === undefined
      ? undefined
      : pod.source_age_ms_at_start === null ? t('report.unknown') : pod.source_age_ms_at_start],
    ['calc.provenance.height_reference_generation', pod.height_reference_generation],
  ]);
}

function pickStatusLabel(status: LocalizationPickRecord['channels'][number]['status'], t: (key: I18nKey) => string): string {
  switch (status) {
    case 'accepted': return t('calc.pick.accepted');
    case 'rejected_no_trigger': return t('calc.pick.no_trigger');
    case 'rejected_no_refinement': return t('calc.pick.no_refinement');
    case 'rejected_low_snr': return t('calc.pick.low_snr');
    case 'rejected_inconsistent': return t('calc.pick.inconsistent');
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
  const { locale } = useLocale();
  const t = useT();
  const [form, setForm] = useState<FormState>(EMPTY_FORM);
  const [error, setError] = useState<string | null>(null);
  const [running, setRunning] = useState(false);
  const [job, setJob] = useState<LocalizationJobStatus | null>(null);
  const [runs, setRuns] = useState<LocalizationRun[] | null>(null);
  const [geometry, setGeometry] = useState<LocalizationSessionGeometry | null>(null);
  const [csPairs, setCsPairs] = useState<LocalizationSessionCsPairs | null>(null);
  const [picks, setPicks] = useState<LocalizationPickRecord[] | null>(null);
  const [geometryError, setGeometryError] = useState<string | null>(null);
  /* MAX-148: drives the unconstrained/scan-constrained toggle; null = still checking. */
  const [hasScan, setHasScan] = useState<boolean | null>(null);
  const provenance = geometry?.provenance;
  const provenanceFields = provenance === undefined ? [] : geometryProvenanceFields(provenance, t);
  const podProvenance = (geometry?.pods ?? []).map((pod) => ({ pod: pod.pod, fields: podProvenanceFields(pod, t) }))
    .filter(({ fields }) => fields.length > 0);

  useEffect(() => {
    let cancelled = false;
    setGeometry(null);
    setCsPairs(null);
    setPicks(null);
    setGeometryError(null);
    setHasScan(null);
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
    fetchScanInfo(objectId, sessionId)
      .then((info) => { if (!cancelled) setHasScan(info.present); })
      /* Best-effort: an unreachable scan-info endpoint just keeps the toggle disabled, never blocks the panel. */
      .catch(() => { if (!cancelled) setHasScan(false); });
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
  /* Optional: blank = the device's default budget (60 s). */
  const timeBudgetS = timeBudgetSeconds(form.timeBudgetS);
  const staLtaIssue = staLtaThresholdIssue(parsed.staWindowSamples, parsed.ltaWindowSamples, parsed.staLtaTriggerRatio, locale);
  const sWave = sWavePickSettings({
    minSeparationSamples: form.sMinSeparationSamples,
    maxSeparationSamples: form.sMaxSeparationSamples,
    staWindowSamples: form.sStaWindowSamples,
    ltaWindowSamples: form.sLtaWindowSamples,
    staLtaTriggerRatio: form.sStaLtaTriggerRatio,
    minimumSnr: form.sMinimumSnr,
  }, locale);
  const formValid =
    Object.values(parsed).every((value) => value !== null) &&
    maxSourceDepthM !== null &&
    timeBudgetS !== null &&
    staLtaIssue === null &&
    sWave.state !== 'invalid';

  const runCalculate = (): void => {
    if (!formValid) return;
    setRunning(true);
    setError(null);
    setRuns(null);
    setJob(null);

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
      ...(sWave.state === 'on' ? sWave.payload : {}),
      ...(timeBudgetS !== undefined ? { time_budget_s: timeBudgetS } : {}),
    })
      .then((result) => {
        /* The device only queued the job (409: one is already running). */
        if (!result.ok) throw new Error(result.error ?? 'calculate_source failed');
        return waitForLocalizationJob(fetchLocalizationJob, {
          objectId,
          sessionId,
          budgetMs: (timeBudgetS ?? DEFAULT_TIME_BUDGET_S) * 1000,
          locale,
          onProgress: setJob,
        });
      })
      .then((finished) => {
        if (finished.state === 'failed') throw new Error(jobProgressText(finished, locale));
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
        <div><span className="eyebrow">{t('calc.session', { id: sessionId })}</span><h2>{t('calc.title')}</h2></div>
        <button disabled={busy || running} onClick={onClose}>{t('calc.close')}</button>
      </div>
      <p>{t('calc.description')}</p>

      <h3>{t('calc.geometry')}</h3>
      {geometryError !== null && <div className="console-alert" role="alert">{geometryError}</div>}
      {geometry === null && geometryError === null && <p>{t('calc.loading_geometry')}</p>}
      {geometry !== null && !geometry.available && (
        <p>{t('calc.no_geometry')}</p>
      )}
      {geometry !== null && geometry.available && (
        <>
          <dl className="console-dl">
            {geometry.pods.map((pod) => (
              <div key={pod.pod}>
                <dt>{t('objects.pod', { number: formatNumber(locale, pod.pod) })}</dt>
                <dd>
                  {pod.computed
                    ? `${formatMm(pod.x_mm, locale)}, ${formatMm(pod.y_mm, locale)}, ${formatMm(pod.z_mm, locale)} · ${pod.leveled ? t('calc.level') : t('calc.not_level')}` +
                      (pod.residual_mm !== undefined ? ` · ${t('calc.cs_residual', { value: formatMm(pod.residual_mm, locale) })}` : '') +
                      (pod.uncertainty_mm === undefined || pod.uncertainty_mm === 0
                        ? ''
                        : pod.uncertainty_mm >= 2147483647
                          ? ` · ${t('calc.position_not_determined')}`
                          : ` · ±${formatMm(pod.uncertainty_mm, locale)}`)
                    : t('calc.not_participating')}
                </dd>
              </div>
            ))}
          </dl>
          {(provenanceFields.length > 0 || podProvenance.length > 0) && (
            <div className="provenance-block">
              <h4>{t('calc.provenance')}</h4>
              {provenanceFields.length > 0 && (
                <dl className="console-dl">
                  {provenanceFields.map(({ key, value }) => <div key={key}><dt>{t(key)}</dt><dd>{value}</dd></div>)}
                </dl>
              )}
              {podProvenance.map(({ pod, fields }) => (
                <div className="session-row" key={`provenance-${pod}`}>
                  <strong>{nodeLabel(pod, locale)}</strong>
                  <small>{fields.map(({ key, value }) => `${t(key)}: ${value}`).join(' · ')}</small>
                </div>
              ))}
            </div>
          )}
        </>
      )}

      {csPairs !== null && csPairs.available && csPairs.pairs.length > 0 && (
        <details className="settings-details">
          <summary>{t('calc.cs_pairs', { count: formatNumber(locale, csPairs.pairs.length) })}</summary>
          <div className="session-list">
            {csPairs.pairs.map((pair) => (
              <div className="session-row" key={`${pair.node_a}-${pair.node_b}`}>
                <span>
                  <strong>{nodeLabel(pair.node_a, locale)} ↔ {nodeLabel(pair.node_b, locale)}</strong>
                  <small>{pair.valid ? t('calc.valid') : t('calc.invalid')} · {formatNumber(locale, pair.attempts)} {pair.attempts === 1 ? t('calc.attempt') : t('calc.attempts')}{pair.state !== undefined ? ` · ${t('calc.provenance.state')}: ${pair.state}` : ''}{pair.last_failure !== undefined ? ` · ${t('calc.provenance.last_failure')}: ${pair.last_failure}` : ''}</small>
                </span>
                <span>
                  <strong>{formatDistance(locale, pair.distance_mm / 1000)}</strong>
                  <small>{t('calc.confidence_value', { value: formatNumber(locale, pair.confidence / 1000, { minimumFractionDigits: 2, maximumFractionDigits: 2 }) })} · {formatNumber(locale, pair.valid_samples)} {t('calc.samples')}{pair.spread_mm !== undefined && pair.valid_samples > 1 ? ` · ${t('calc.spread', { value: formatDistance(locale, pair.spread_mm / 1000) })}` : ''}{pair.rejected_samples ? ` · ${t('calc.outliers_dropped', { count: formatNumber(locale, pair.rejected_samples) })}` : ''}{pair.provider_quality_flags !== undefined ? ` · ${t('calc.provenance.provider_quality_flags')}: ${pair.provider_quality_flags}` : ''}{pair.scheduler_quality_flags !== undefined ? ` · ${t('calc.provenance.scheduler_quality_flags')}: ${pair.scheduler_quality_flags}` : ''}</small>
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
            <summary>{t('calc.arrival_picks', { count: formatNumber(locale, picks.length) })}</summary>
            <div className="session-list">
              {latest.channels.map((channel) => (
                <div className="session-row" key={channel.channel}>
                  <span>
                    <strong>{t('calc.channel', { number: formatNumber(locale, channel.channel) })}</strong>
                    <small>{channel.participating ? pickStatusLabel(channel.status, t) : t('calc.not_participating')}</small>
                  </span>
                  <span>
                    <strong>{channel.status === 'accepted' ? `${formatNumber(locale, channel.arrival_time_s, { minimumFractionDigits: 3, maximumFractionDigits: 3 })} s` : '—'}</strong>
                    <small>{t('calc.snr_value', { value: formatNumber(locale, channel.snr, { minimumFractionDigits: 1, maximumFractionDigits: 1 }) })}</small>
                  </span>
                </div>
              ))}
            </div>
          </details>
        );
      })()}

      <div className="cable-grid">
        <label>
          <span>{t('calc.propagation_speed')}</span>
          <input type="number" min="0" step="any" value={form.propagationSpeedMps} onChange={set('propagationSpeedMps')} />
        </label>
        <label>
          <span>{t('calc.speed_uncertainty')}</span>
          <input type="number" min="0" step="any" value={form.propagationSpeedUncertaintyMps} onChange={set('propagationSpeedUncertaintyMps')} />
        </label>
        <label>
          <span>{t('calc.model_name')}</span>
          <input type="text" maxLength={31} value={form.propagationModelName} onChange={set('propagationModelName')} />
        </label>
        <label>
          <span>{t('calc.timing_uncertainty')}</span>
          <input type="number" min="0" step="any" value={form.timingUncertaintyUs} onChange={set('timingUncertaintyUs')} />
        </label>
      </div>

      <details className="settings-details">
        <summary>{t('calc.pick_config')}</summary>
        <div className="cable-grid">
          <label>
            <span>{t('calc.noise_window')}</span>
            <input type="number" min="1" step="1" value={form.noiseWindowSamples} onChange={set('noiseWindowSamples')} />
          </label>
          <label>
            <span>{t('calc.sta_window')}</span>
            <input type="number" min="1" step="1" value={form.staWindowSamples} onChange={set('staWindowSamples')} />
          </label>
          <label>
            <span>{t('calc.lta_window')}</span>
            <input type="number" min="1" step="1" value={form.ltaWindowSamples} onChange={set('ltaWindowSamples')} />
          </label>
          <label>
            <span>{t('calc.trigger_ratio')}</span>
            <input type="number" min="0" step="any" value={form.staLtaTriggerRatio} onChange={set('staLtaTriggerRatio')} />
          </label>
          <label>
            <span>{t('calc.refine_lookback')}</span>
            <input type="number" min="1" step="1" value={form.refineLookbackSamples} onChange={set('refineLookbackSamples')} />
          </label>
          <label>
            <span>{t('calc.refine_lookahead')}</span>
            <input type="number" min="1" step="1" value={form.refineLookaheadSamples} onChange={set('refineLookaheadSamples')} />
          </label>
          <label>
            <span>{t('calc.refine_fraction')}</span>
            <input type="number" min="0" max="1" step="any" value={form.refineAmplitudeFraction} onChange={set('refineAmplitudeFraction')} />
          </label>
          <label>
            <span>{t('calc.snr_window')}</span>
            <input type="number" min="1" step="1" value={form.snrSignalWindowSamples} onChange={set('snrSignalWindowSamples')} />
          </label>
          <label title={t('calc.minimum_snr_help')}>
            <span>{t('calc.minimum_snr')}</span>
            <input type="number" min="0" step="any" value={form.minimumSnr} onChange={set('minimumSnr')} />
          </label>
          <label>
            <span>{t('calc.semblance_window')}</span>
            <input type="number" min="1" step="1" value={form.semblanceWindowHalfSpanSamples} onChange={set('semblanceWindowHalfSpanSamples')} />
          </label>
          <label title={t('calc.max_depth_help')}>
            <span>{t('calc.max_depth')}</span>
            <input type="number" min="0" step="any" value={form.maxSourceDepthM} onChange={set('maxSourceDepthM')} />
          </label>
          <label title={t('calc.time_budget_help')}>
            <span>{t('calc.time_budget')}</span>
            <input type="number" min="1" max="600" step="any" value={form.timeBudgetS} onChange={set('timeBudgetS')} />
          </label>
        </div>
        {staLtaIssue !== null && <div className="console-alert" role="alert">{staLtaIssue}</div>}
        {maxSourceDepthM === null && <div className="console-alert" role="alert">{t('calc.max_depth_error')}</div>}
        {timeBudgetS === null && <div className="console-alert" role="alert">{t('calc.time_budget_error')}</div>}
      </details>

      <details className="settings-details">
        <summary>{t('calc.swave_config')}</summary>
        <div className="cable-grid">
          <label title={t('calc.s_min_help')}>
            <span>{t('calc.s_min_separation')}</span>
            <input type="number" min="0" step="1" value={form.sMinSeparationSamples} onChange={set('sMinSeparationSamples')} />
          </label>
          <label title={t('calc.s_max_help')}>
            <span>{t('calc.s_max_separation')}</span>
            <input type="number" min="1" step="1" value={form.sMaxSeparationSamples} onChange={set('sMaxSeparationSamples')} />
          </label>
          <label>
            <span>{t('calc.s_sta_window')}</span>
            <input type="number" min="1" step="1" value={form.sStaWindowSamples} onChange={set('sStaWindowSamples')} />
          </label>
          <label title={t('calc.s_lta_help')}>
            <span>{t('calc.s_lta_window')}</span>
            <input type="number" min="1" step="1" value={form.sLtaWindowSamples} onChange={set('sLtaWindowSamples')} />
          </label>
          <label>
            <span>{t('calc.s_trigger_ratio')}</span>
            <input type="number" min="0" step="any" value={form.sStaLtaTriggerRatio} onChange={set('sStaLtaTriggerRatio')} />
          </label>
          <label title={t('calc.s_minimum_snr_help')}>
            <span>{t('calc.s_minimum_snr')}</span>
            <input type="number" min="0" step="any" value={form.sMinimumSnr} onChange={set('sMinimumSnr')} />
          </label>
        </div>
        {sWave.state === 'invalid' && <div className="console-alert" role="alert">{sWave.issue}</div>}
      </details>

      {error !== null && <div className="console-alert" role="alert">{error}</div>}

      <div className="console-actions">
        <button disabled={busy || running || !formValid} onClick={runCalculate}>
          {running ? t('calc.loading') : t('calc.calculate')}
        </button>
      </div>

      {job !== null && <LocalizationJobReport status={job} />}

      {runs !== null && runs.length > 0 && (
        <LocalizationOverlay runs={runs} geometry={geometry} hasScan={hasScan} />
      )}

      {runs !== null && (
        <>
        <h3>{t('calc.runs')}</h3>
        <div className="session-list">
          {runs.length === 0 ? (
            <p>{t('calc.no_runs')}</p>
          ) : runs.map((run) => (
            <div className="session-row" key={`${run.method_name}-${run.computed_at_ms}`}>
              <span>
                <strong>{run.method_name}</strong>
                <small>{run.kind === 'joint_fusion' ? t('calc.joint_fusion') : run.dimensionality}</small>
              </span>
              <span>
                <strong>{run.solved ? formatPosition(run, locale) : t('calc.unsolved')}</strong>
                <small>
                  {run.solved
                    ? t('calc.confidence_value', { value: formatNumber(locale, run.confidence_score, { minimumFractionDigits: 2, maximumFractionDigits: 2 }) }) +
                      (run.degrees_of_freedom === 0 ? ` · ${t('calc.unverified')}` : '') +
                      estimatedVelocityText(run, locale) +
                      (uncertaintyText(run, locale) === '' ? '' : ` · ${uncertaintyText(run, locale)}`) +
                      (run.method_name === 'energy_baseline' ? ` · ${t('calc.diagnostic_only')}` : '')
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
