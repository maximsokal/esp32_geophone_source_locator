import { useEffect, useState, type ChangeEvent } from 'react';
import {
  getCalcSettings,
  saveCalcSettings,
  type CalcSettingsProfile,
  type CalcSettingsResponse,
  type LocalizationVelocityRange,
  type SaveCalcSettingsPayload,
} from '../../api/deviceApi';
import { useLocale, useT, type I18nKey } from '../../i18n';
import { formatNumber } from '../../i18n/format';
import { materialNameKey } from './material';

interface Props {
  objectId: string;
  ready?: boolean;
  onSettingsLoaded?: (settings: CalcSettingsProfile | null) => void;
  onSaved?: () => void | Promise<unknown>;
}

const ADVANCED_FIELDS: Array<{
  key: string;
  label: I18nKey;
  type?: 'text';
  step?: string;
  min?: number;
  max?: number;
}> = [
  { key: 'propagation_model_name', label: 'calc.model_name', type: 'text' },
  { key: 'timing_uncertainty_us', label: 'calc.timing_uncertainty', min: 0, step: 'any' },
  { key: 'noise_window_samples', label: 'calc.noise_window', min: 1, step: '1' },
  { key: 'sta_window_samples', label: 'calc.sta_window', min: 1, step: '1' },
  { key: 'lta_window_samples', label: 'calc.lta_window', min: 1, step: '1' },
  { key: 'sta_lta_trigger_ratio', label: 'calc.trigger_ratio', min: 0, step: 'any' },
  { key: 'refine_lookback_samples', label: 'calc.refine_lookback', min: 1, step: '1' },
  { key: 'refine_lookahead_samples', label: 'calc.refine_lookahead', min: 1, step: '1' },
  { key: 'refine_amplitude_fraction', label: 'calc.refine_fraction', min: 0, max: 1, step: 'any' },
  { key: 'snr_signal_window_samples', label: 'calc.snr_window', min: 1, step: '1' },
  { key: 'minimum_snr', label: 'calc.minimum_snr', min: 0, step: 'any' },
  { key: 'semblance_window_half_span_samples', label: 'calc.semblance_window', min: 1, step: '1' },
  { key: 'max_source_depth_m', label: 'calc.max_depth', min: 0, step: 'any' },
  { key: 'time_budget_s', label: 'calc.time_budget', min: 1, max: 600, step: 'any' },
  { key: 's_min_separation_samples', label: 'calc.s_min_separation', min: 0, step: '1' },
  { key: 's_max_separation_samples', label: 'calc.s_max_separation', min: 1, step: '1' },
  { key: 's_sta_window_samples', label: 'calc.s_sta_window', min: 1, step: '1' },
  { key: 's_lta_window_samples', label: 'calc.s_lta_window', min: 1, step: '1' },
  { key: 's_sta_lta_trigger_ratio', label: 'calc.s_trigger_ratio', min: 0, step: 'any' },
  { key: 's_minimum_snr', label: 'calc.s_minimum_snr', min: 0, step: 'any' },
];

function inputText(value: unknown): string {
  return typeof value === 'number' || typeof value === 'string' || typeof value === 'boolean' ? String(value) : '';
}

function numberValue(value: string): number | null {
  if (value.trim() === '') return null;
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : null;
}

/** MAX-301: the stored velocity range, when the device reports one. */
function velocityRangeOf(settings: CalcSettingsProfile | null | undefined): LocalizationVelocityRange | undefined {
  const raw = (settings as { velocity_range?: unknown } | null | undefined)?.velocity_range;
  if (typeof raw !== 'object' || raw === null) return undefined;
  const range = raw as Partial<LocalizationVelocityRange>;
  return typeof range.v_min_mps === 'number' && typeof range.v_max_mps === 'number'
    ? range as LocalizationVelocityRange
    : undefined;
}

/** Mirrors the firmware rule: 50 <= min <= max <= 8000 and max/min <= 10. */
function velocityRangeValid(min: number | null, max: number | null): boolean {
  return min !== null && max !== null && min >= 50 && max <= 8000 && min <= max && max / min <= 10;
}

function initialAdvanced(response: CalcSettingsResponse): Record<string, string> {
  const source = response.settings ?? response.standardProcessing;
  return {
    s_pick_enabled: inputText(source.s_pick_enabled),
    ...Object.fromEntries(ADVANCED_FIELDS.map(({ key }) => [key, inputText(source[key])])),
  };
}

function changedAdvanced(
  values: Record<string, string>,
  baseline: CalcSettingsProfile | CalcSettingsResponse['standardProcessing'],
): Record<string, string | number | boolean> {
  const changed: Record<string, string | number | boolean> = {};
  const sEnabled = values.s_pick_enabled === 'true';
  if (sEnabled !== Boolean(baseline.s_pick_enabled)) changed.s_pick_enabled = sEnabled;
  for (const field of ADVANCED_FIELDS) {
    if (field.key.startsWith('s_') && !sEnabled) continue;
    const text = values[field.key]?.trim() ?? '';
    if (text === '') continue;
    const current = field.type === 'text' ? text : numberValue(text);
    if (current === null) continue;
    const previous = baseline[field.key];
    if (field.type === 'text' ? current !== previous : Number(previous) !== current) {
      changed[field.key] = current;
    }
  }
  return changed;
}

export default function MaterialSettingsForm({
  objectId,
  ready = false,
  onSettingsLoaded,
  onSaved,
}: Props) {
  const t = useT();
  const { locale } = useLocale();
  const [response, setResponse] = useState<CalcSettingsResponse | null>(null);
  const [material, setMaterial] = useState('');
  const [speed, setSpeed] = useState('');
  const [uncertainty, setUncertainty] = useState('');
  const [advanced, setAdvanced] = useState<Record<string, string>>({});
  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);
  const [editing, setEditing] = useState(false);
  const [loadError, setLoadError] = useState<string | null>(null);
  const [saveError, setSaveError] = useState<string | null>(null);
  const [saved, setSaved] = useState(false);
  const [convertedToCustom, setConvertedToCustom] = useState(false);
  /* MAX-301: an explicit custom range; empty = let the device derive it. */
  const [rangeMin, setRangeMin] = useState('');
  const [rangeMax, setRangeMax] = useState('');

  const applyResponse = (next: CalcSettingsResponse): void => {
    setResponse(next);
    setMaterial(next.settings?.material ?? '');
    setSpeed(inputText(next.settings?.propagation_speed_mps));
    setUncertainty(inputText(next.settings?.propagation_speed_uncertainty_mps));
    const storedRange = velocityRangeOf(next.settings);
    const userRange = storedRange?.source === 'user' ? storedRange : undefined;
    setRangeMin(inputText(userRange?.v_min_mps));
    setRangeMax(inputText(userRange?.v_max_mps));
    setAdvanced(initialAdvanced(next));
    setConvertedToCustom(false);
    onSettingsLoaded?.(next.settings);
  };

  useEffect(() => {
    let cancelled = false;
    setLoading(true);
    setLoadError(null);
    void getCalcSettings(objectId).then((next) => {
      if (cancelled) return;
      applyResponse(next);
    }).catch((reason: unknown) => {
      if (!cancelled) setLoadError(reason instanceof Error ? reason.message : String(reason));
    }).finally(() => {
      if (!cancelled) setLoading(false);
    });
    return () => { cancelled = true; };
  }, [objectId]);

  const selectMaterial = (event: ChangeEvent<HTMLSelectElement>): void => {
    const nextMaterial = event.target.value;
    setMaterial(nextMaterial);
    setConvertedToCustom(false);
    setSaveError(null);
    setSaved(false);
    if (nextMaterial !== 'custom') {
      const preset = response?.presets.find((item) => item.id === nextMaterial);
      if (preset !== undefined) {
        setSpeed(String(preset.propagation_speed_mps));
        setUncertainty(String(preset.propagation_speed_uncertainty_mps));
      }
    }
  };

  const editMaterialValue = (field: 'speed' | 'uncertainty' | 'rangeMin' | 'rangeMax', value: string): void => {
    if (field === 'speed') setSpeed(value);
    else if (field === 'uncertainty') setUncertainty(value);
    else if (field === 'rangeMin') setRangeMin(value);
    else setRangeMax(value);
    if (material !== '' && material !== 'custom') {
      setMaterial('custom');
      setConvertedToCustom(true);
    }
    setSaveError(null);
    setSaved(false);
  };

  const baseline = response?.standardProcessing;
  const speedValue = numberValue(speed);
  const uncertaintyValue = numberValue(uncertainty);
  const sta = numberValue(advanced.sta_window_samples ?? '');
  const lta = numberValue(advanced.lta_window_samples ?? '');
  const ratio = numberValue(advanced.sta_lta_trigger_ratio ?? '');
  const sMin = numberValue(advanced.s_min_separation_samples ?? '');
  const sMax = numberValue(advanced.s_max_separation_samples ?? '');
  const sSta = numberValue(advanced.s_sta_window_samples ?? '');
  const sLta = numberValue(advanced.s_lta_window_samples ?? '');
  const sRatio = numberValue(advanced.s_sta_lta_trigger_ratio ?? '');
  const sMinimumSnr = numberValue(advanced.s_minimum_snr ?? '');
  const sEnabled = advanced.s_pick_enabled === 'true';
  const validation: I18nKey[] = [];
  if (material !== '') {
    if (speedValue === null || speedValue <= 50 || speedValue >= 8000) {
      validation.push('workflow.material.validation.speed');
    }
    if (uncertaintyValue === null || uncertaintyValue <= 0 ||
        speedValue === null || uncertaintyValue >= speedValue) {
      validation.push('workflow.material.validation.uncertainty');
    }
  }
  const rangeMinValue = numberValue(rangeMin);
  const rangeMaxValue = numberValue(rangeMax);
  const rangeEntered = rangeMin.trim() !== '' || rangeMax.trim() !== '';
  if (rangeEntered && !velocityRangeValid(rangeMinValue, rangeMaxValue)) {
    validation.push('workflow.material.validation.range');
  }
  if (response !== null && (sta === null || lta === null || sta <= 0 || lta <= sta)) {
    validation.push('workflow.material.validation.sta_lta');
  } else if (response !== null && (ratio === null || ratio <= 0 || ratio >= (lta as number) / (sta as number))) {
    validation.push('workflow.material.validation.ratio');
  }
  if (sEnabled && (sMin === null || sMax === null || sMax <= sMin || sSta === null || sLta === null ||
      sSta <= 0 || sLta <= sSta || sRatio === null || sRatio <= 0 || sRatio >= sLta / sSta ||
      sMinimumSnr === null || sMinimumSnr < 0)) {
    validation.push('workflow.material.validation.s_wave');
  }

  const save = async (): Promise<void> => {
    if (response === null || baseline === undefined || material === '' || validation.length > 0) return;
    const payload: SaveCalcSettingsPayload = { material };
    if (material === 'custom') {
      if (speedValue === null || uncertaintyValue === null) return;
      payload.propagation_speed_mps = speedValue;
      payload.propagation_speed_uncertainty_mps = uncertaintyValue;
      if (rangeEntered && rangeMinValue !== null && rangeMaxValue !== null) {
        payload.velocity_range = {
          v_min_mps: rangeMinValue,
          v_max_mps: rangeMaxValue,
          vp_vs_min: 0,
          vp_vs_max: 0,
          source: 'user',
        };
      }
    }
    Object.assign(payload, changedAdvanced(advanced, baseline));
    setSaving(true);
    setSaveError(null);
    setSaved(false);
    const result = await saveCalcSettings(objectId, payload);
    if (!result.ok) {
      setSaveError(t('workflow.material.save_error', { error: result.error, status: result.httpStatus }));
      setSaving(false);
      return;
    }
    setSaved(true);
    try {
      const refreshed = await getCalcSettings(objectId);
      applyResponse(refreshed);
    } catch (reason) {
      setLoadError(reason instanceof Error ? reason.message : String(reason));
    }
    await onSaved?.();
    setEditing(false);
    setSaving(false);
  };

  const settings = response?.settings;
  const showSummary = ready && !editing && settings !== null && settings !== undefined;
  if (showSummary) {
    const name = t(materialNameKey(settings.material));
    return (
      <section className="workflow-material-summary">
        <p>{t('workflow.material.current', {
          name,
          speed: formatNumber(locale, settings.propagation_speed_mps),
          sigma: formatNumber(locale, settings.propagation_speed_uncertainty_mps),
        })}</p>
        {velocityRangeOf(settings) !== undefined && (
          <p>{t('workflow.material.range_current', {
            min: formatNumber(locale, velocityRangeOf(settings)?.v_min_mps ?? 0),
            max: formatNumber(locale, velocityRangeOf(settings)?.v_max_mps ?? 0),
          })}</p>
        )}
        <button type="button" className="secondary" onClick={() => setEditing(true)}>
          {t('workflow.material.change')}
        </button>
      </section>
    );
  }

  const disabled = loading || saving || response === null || material === '' || validation.length > 0;
  return (
    <section className="workflow-material-settings">
      <h3>{t('workflow.material.title')}</h3>
      {loading && <p role="status">{t('workflow.result.loading')}</p>}
      {loadError !== null && <p className="console-error" role="alert">{loadError}</p>}
      {response !== null && (
        <>
          <label>
            <span>{t('workflow.material.label')}</span>
            <select value={material} onChange={selectMaterial} aria-label={t('workflow.material.label')}>
              <option value="">{t('workflow.material.choose')}</option>
              {response.presets.map((preset) => (
                <option key={preset.id} value={preset.id}>
                  {t(materialNameKey(preset.id))} ({formatNumber(locale, preset.propagation_speed_mps)} ± {formatNumber(locale, preset.propagation_speed_uncertainty_mps)} m/s)
                </option>
              ))}
              <option value="custom">{t(materialNameKey('custom'))}</option>
            </select>
          </label>
          <div className="cable-grid">
            <label>
              <span>{t('calc.propagation_speed')}</span>
              <input
                aria-label={t('calc.propagation_speed')}
                type="number"
                min="50"
                max="8000"
                step="any"
                disabled={loading || saving || material === ''}
                value={speed}
                onChange={(event) => editMaterialValue('speed', event.target.value)}
              />
            </label>
            <label>
              <span>{t('calc.speed_uncertainty')}</span>
              <input
                aria-label={t('calc.speed_uncertainty')}
                type="number"
                min="0"
                step="any"
                disabled={loading || saving || material === ''}
                value={uncertainty}
                onChange={(event) => editMaterialValue('uncertainty', event.target.value)}
              />
            </label>
            <label>
              <span>{t('workflow.material.range_min')}</span>
              <input
                aria-label={t('workflow.material.range_min')}
                type="number"
                min="50"
                max="8000"
                step="any"
                placeholder={t('workflow.material.range_auto')}
                disabled={loading || saving || material === ''}
                value={rangeMin}
                onChange={(event) => editMaterialValue('rangeMin', event.target.value)}
              />
            </label>
            <label>
              <span>{t('workflow.material.range_max')}</span>
              <input
                aria-label={t('workflow.material.range_max')}
                type="number"
                min="50"
                max="8000"
                step="any"
                placeholder={t('workflow.material.range_auto')}
                disabled={loading || saving || material === ''}
                value={rangeMax}
                onChange={(event) => editMaterialValue('rangeMax', event.target.value)}
              />
            </label>
          </div>
          {material !== '' && material !== 'custom' && <p>{t('workflow.material.edit_hint')}</p>}
          {convertedToCustom && <p role="status">{t('workflow.material.switched_to_custom')}</p>}
          <details className="settings-details">
            <summary>{t('workflow.material.advanced')}</summary>
            <div className="cable-grid">
              <label>
                <span>{t('workflow.material.s_pick_enabled')}</span>
                <input
                  aria-label={t('workflow.material.s_pick_enabled')}
                  type="checkbox"
                  checked={sEnabled}
                  disabled={loading || saving}
                  onChange={(event) => {
                    setAdvanced((previous) => ({ ...previous, s_pick_enabled: String(event.target.checked) }));
                    setSaveError(null);
                    setSaved(false);
                  }}
                />
              </label>
              {ADVANCED_FIELDS.map((field) => (
                field.key.startsWith('s_') && !sEnabled ? null :
                <label key={field.key}>
                  <span>{t(field.label)}</span>
                  <input
                    aria-label={t(field.label)}
                    type={field.type ?? 'number'}
                    min={field.min}
                    max={field.max}
                    step={field.step}
                    disabled={loading || saving || material === ''}
                    value={advanced[field.key] ?? ''}
                    onChange={(event) => {
                      setAdvanced((previous) => ({ ...previous, [field.key]: event.target.value }));
                      setSaveError(null);
                      setSaved(false);
                    }}
                  />
                </label>
              ))}
            </div>
            {response !== null && validation.includes('workflow.material.validation.sta_lta') &&
              <p className="console-alert" role="alert">{t('workflow.material.validation.sta_lta')}</p>}
            {response !== null && validation.includes('workflow.material.validation.ratio') &&
              <p className="console-alert" role="alert">{t('workflow.material.validation.ratio')}</p>}
          </details>
          {validation.map((key) => <p className="console-alert" role="alert" key={key}>{t(key)}</p>)}
          {saveError !== null && <p className="console-error" role="alert">{saveError}</p>}
          {saved && <p role="status">{t('workflow.material.saved')}</p>}
          <button type="button" disabled={disabled} onClick={() => { void save(); }}>
            {saving ? t('workflow.material.saving') : t('workflow.material.save')}
          </button>
        </>
      )}
    </section>
  );
}
