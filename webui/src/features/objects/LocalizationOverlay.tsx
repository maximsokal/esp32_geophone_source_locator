import { Suspense, lazy, useMemo, useState } from 'react';

import type { LocalizationRun, LocalizationSessionGeometry } from '../../api/deviceApi';
import {
  buildLocalizationOverlay,
  dimensionalityLabel,
  filterOverlayByVariant,
  localizationMachineAttributes,
  overlappingSectors,
  selectOverlayMethod,
  toggleAvailability,
  uncertaintyText,
  type ResultVariant,
} from '../../domain/localizationOverlay';
import { translate, useLocale, useT } from '../../i18n';
import { formatDistance, formatNumber } from '../../i18n/format';

const LocalizationOverlayScene = lazy(() => import('./LocalizationOverlayScene'));

interface Props {
  runs: LocalizationRun[];
  geometry: LocalizationSessionGeometry | null;
  /**
   * MAX-148: whether this session has a registered 3D scan (drives the
   * unconstrained/scan-constrained toggle). null while still loading.
   */
  hasScan?: boolean | null;
  /** Tests render without WebGL. */
  renderScene?: boolean;
}

/** The latest run of a method's posterior summary, or a dash. */
function uncertaintyCell(runs: LocalizationRun[], method: string, locale: 'en' | 'uk'): string {
  const latest = runs
    .filter((run) => run.method_name === method)
    .reduce<LocalizationRun | null>((best, run) => (best === null || run.computed_at_ms > best.computed_at_ms ? run : best), null);
  const text = latest === null ? '' : uncertaintyText(latest, locale);
  return text === '' ? '—' : text;
}

/** MAX-301: the latest run's verdict on the velocity range, when it warns. */
function velocityNote(runs: LocalizationRun[], method: string, locale: 'en' | 'uk'): string {
  const latest = runs
    .filter((run) => run.method_name === method)
    .reduce<LocalizationRun | null>((best, run) => (best === null || run.computed_at_ms > best.computed_at_ms ? run : best), null);
  if (latest?.velocity_status === 'inconsistent') return ` — ${translate(locale, 'overlay.velocity_inconsistent')}`;
  if (latest?.velocity_status === 'at_bound') return ` — ${translate(locale, 'overlay.velocity_at_bound')}`;
  return '';
}

function formatPosition(x: number, y: number, z: number, locale: 'en' | 'uk'): string {
  const digits = { minimumFractionDigits: 2, maximumFractionDigits: 2 };
  return `${formatNumber(locale, x, digits)}, ${formatNumber(locale, y, digits)}, ${formatNumber(locale, z, digits)} m`;
}

function unavailableReasonLabel(reason: string, t: ReturnType<typeof useT>): string {
  switch (reason) {
    case 'session_gap': return t('calc.reason.session_gap');
    case 'timestamp_non_monotonic': return t('calc.reason.timestamp_non_monotonic');
    case 'duplicate_sensor_position': return t('calc.reason.duplicate_sensor_position');
    case 'reflected_transform': return t('calc.reason.reflected_transform');
    default: return reason;
  }
}

/**
 * MAX-161: every persisted method of this recording in one scene; the
 * selected method is vivid, the others dim. Selecting never recomputes.
 */
export default function LocalizationOverlay({ runs, geometry, hasScan = null, renderScene = true }: Props) {
  const t = useT();
  const { locale } = useLocale();
  const base = useMemo(() => buildLocalizationOverlay(runs, geometry, null), [runs, geometry]);
  const availability = useMemo(() => toggleAvailability(runs, hasScan), [runs, hasScan]);
  const [variant, setVariant] = useState<ResultVariant>('unconstrained');
  const [selected, setSelected] = useState<string | null>(null);

  const variantModel = availability.available ? filterOverlayByVariant(base, variant) : base;
  const model = selected === null ? variantModel : selectOverlayMethod(variantModel, selected);
  const overlaps = overlappingSectors(model);
  const unavailableReason = availability.reason === null ? null : t(({
    checking_scan: 'overlay.checking_scan',
    no_scan: 'overlay.no_scan',
    no_constrained_results: 'overlay.no_constrained_results',
  } as const)[availability.reason as 'checking_scan' | 'no_scan' | 'no_constrained_results'] ?? 'overlay.checking_scan');

  if (base.sectors.length === 0 && base.unavailable.length === 0) return null;

  const chooseVariant = (next: ResultVariant) => {
    setVariant(next);
    setSelected(null);
  };

  return (
    <section className="localization-overlay" aria-label={t('overlay.aria_label')}>
      <div className="card-heading">
        <div><span className="eyebrow">{t('overlay.source_estimates')}</span><h3>{t('overlay.title')}</h3></div>
        {model.sectors.length > 0 && (
          <label className="overlay-select">
            {t('overlay.method')}
            <select
              value={model.selectedMethod ?? ''}
              onChange={(event) => setSelected(event.target.value)}
            >
              {model.sectors.map((sector) => (
                <option key={sector.method} value={sector.method}>{sector.method}</option>
              ))}
            </select>
          </label>
        )}
      </div>

      <fieldset className="overlay-variant-toggle" disabled={!availability.available}>
        <legend>{t('overlay.result_variant')}</legend>
        <label>
          <input
            type="radio"
            name="overlay-result-variant"
            value="unconstrained"
            checked={variant === 'unconstrained'}
            disabled={!availability.available}
            onChange={() => chooseVariant('unconstrained')}
          />
          {t('overlay.unconstrained')}
        </label>
        <label>
          <input
            type="radio"
            name="overlay-result-variant"
            value="scan_constrained"
            checked={variant === 'scan_constrained'}
            disabled={!availability.available}
            onChange={() => chooseVariant('scan_constrained')}
          />
          {t('overlay.scan_constrained')}
        </label>
        {unavailableReason !== null && <span className="overlay-variant-reason">{unavailableReason}</span>}
      </fieldset>

      {model.sectors.length === 0 && model.unavailable.length === 0 && (
        <p className="overlay-note">{t('overlay.no_results', { variant: t(variant === 'scan_constrained' ? 'overlay.scan_constrained' : 'overlay.unconstrained') })}</p>
      )}
      {model.sectors.length > 0 && renderScene && (
        <Suspense fallback={<p>{t('overlay.loading_3d')}</p>}>
          <LocalizationOverlayScene model={model} />
        </Suspense>
      )}
      <p className="overlay-note">
        {t('overlay.note')}
      </p>

      {(model.sectors.length > 0 || model.unavailable.length > 0) && (
      <table className="overlay-table">
        <thead>
          <tr><th>{t('overlay.method')}</th><th>{t('overlay.source_xyz')}</th><th>{t('overlay.radius')}</th><th>{t('overlay.confidence')}</th><th>{t('overlay.residual')}</th><th>{t('overlay.uncertainty')}</th><th>{t('overlay.dimension')}</th></tr>
        </thead>
        <tbody>
          {model.sectors.map((sector) => (
            <tr key={sector.method} {...localizationMachineAttributes(sector)} className={sector.emphasis === 'selected' ? 'selected' : 'dim'} data-state={sector.velocityStatus === 'inconsistent' ? 'velocity_inconsistent' : sector.velocityStatus === 'at_bound' ? 'velocity_at_bound' : 'solved'} data-severity={sector.velocityStatus === 'inconsistent' || sector.velocityStatus === 'at_bound' ? 'warning' : 'normal'}>
              <td><span className="method-swatch" style={{ background: sector.color }} />{sector.method}{velocityNote(runs, sector.method, locale)}</td>
              <td>{formatPosition(sector.position.x_m, sector.position.y_m, sector.position.z_m, locale)}</td>
              <td>{formatDistance(locale, sector.radiusM)}{sector.radiusSource === 'nominal' ? ` (${t('overlay.nominal')})` : sector.radiusSource === 'posterior' ? ` (${t('overlay.two_sd')})` : ''}</td>
              <td>{formatNumber(locale, sector.confidence, { minimumFractionDigits: 2, maximumFractionDigits: 2 })}</td>
              <td>{sector.rmsResidualUs === null ? '—' : `${formatNumber(locale, sector.rmsResidualUs)} µs`}</td>
              <td>{uncertaintyCell(runs, sector.method, locale)}</td>
              <td>{dimensionalityLabel(sector.dimensionality, locale)}</td>
            </tr>
          ))}
          {model.unavailable.map((entry) => (
            <tr key={`unavailable-${entry.method}`} className="unavailable" data-state="rejected" data-severity="warning">
              <td>{entry.method}</td>
              <td colSpan={6}>{t('overlay.unavailable_reason', { reason: unavailableReasonLabel(entry.reason, t) })}</td>
            </tr>
          ))}
        </tbody>
      </table>
      )}
      {overlaps.length > 0 && (
        <p className="overlay-note">
          {t('overlay.overlapping', { methods: overlaps.map(([a, b]) => `${a} ↔ ${b}`).join(', ') })}
        </p>
      )}
    </section>
  );
}
