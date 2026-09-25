import { Suspense, lazy, useMemo, useState } from 'react';

import type { LocalizationRun, LocalizationSessionGeometry } from '../../api/deviceApi';
import {
  buildLocalizationOverlay,
  dimensionalityLabel,
  filterOverlayByVariant,
  overlappingSectors,
  selectOverlayMethod,
  toggleAvailability,
  uncertaintyText,
  type ResultVariant,
} from '../../domain/localizationOverlay';

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
function uncertaintyCell(runs: LocalizationRun[], method: string): string {
  const latest = runs
    .filter((run) => run.method_name === method)
    .reduce<LocalizationRun | null>((best, run) => (best === null || run.computed_at_ms > best.computed_at_ms ? run : best), null);
  const text = latest === null ? '' : uncertaintyText(latest);
  return text === '' ? '—' : text;
}

function formatPosition(x: number, y: number, z: number): string {
  return `${x.toFixed(2)}, ${y.toFixed(2)}, ${z.toFixed(2)} m`;
}

/**
 * MAX-161: every persisted method of this recording in one scene; the
 * selected method is vivid, the others dim. Selecting never recomputes.
 */
export default function LocalizationOverlay({ runs, geometry, hasScan = null, renderScene = true }: Props) {
  const base = useMemo(() => buildLocalizationOverlay(runs, geometry, null), [runs, geometry]);
  const availability = useMemo(() => toggleAvailability(runs, hasScan), [runs, hasScan]);
  const [variant, setVariant] = useState<ResultVariant>('unconstrained');
  const [selected, setSelected] = useState<string | null>(null);

  const variantModel = availability.available ? filterOverlayByVariant(base, variant) : base;
  const model = selected === null ? variantModel : selectOverlayMethod(variantModel, selected);
  const overlaps = overlappingSectors(model);

  if (base.sectors.length === 0 && base.unavailable.length === 0) return null;

  const chooseVariant = (next: ResultVariant) => {
    setVariant(next);
    setSelected(null);
  };

  return (
    <section className="localization-overlay" aria-label="Localization method comparison">
      <div className="card-heading">
        <div><span className="eyebrow">SOURCE ESTIMATES</span><h3>Method comparison</h3></div>
        {model.sectors.length > 0 && (
          <label className="overlay-select">
            Method
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
        <legend>Result variant</legend>
        <label>
          <input
            type="radio"
            name="overlay-result-variant"
            value="unconstrained"
            checked={variant === 'unconstrained'}
            disabled={!availability.available}
            onChange={() => chooseVariant('unconstrained')}
          />
          Unconstrained
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
          Scan-constrained
        </label>
        {availability.reason !== null && <span className="overlay-variant-reason">{availability.reason}</span>}
      </fieldset>

      {model.sectors.length === 0 && model.unavailable.length === 0 && (
        <p className="overlay-note">No {variant === 'scan_constrained' ? 'scan-constrained' : 'unconstrained'} results for this recording.</p>
      )}
      {model.sectors.length > 0 && renderScene && (
        <Suspense fallback={<p>Loading 3D view…</p>}>
          <LocalizationOverlayScene model={model} />
        </Suspense>
      )}
      <p className="overlay-note">
        Sensor frame of this recording (MAIN at the origin, metres). The object scan is not drawn under the sectors until it is registered to this frame (AprilTag registration, MAX-145).
        Sector radius = RMS time residual × propagation speed; for Bayesian runs twice the largest posterior standard
        deviation, with the credible region drawn as a box; “nominal” means the method reported neither.
        A 2D (planar, 3-REMOTE) result never solved its own height -- it is drawn as a flat disc, not a positioned sphere.
      </p>

      {(model.sectors.length > 0 || model.unavailable.length > 0) && (
      <table className="overlay-table">
        <thead>
          <tr><th>Method</th><th>Source (x, y, z)</th><th>Radius</th><th>Confidence</th><th>Residual</th><th>Uncertainty</th><th>Dim.</th></tr>
        </thead>
        <tbody>
          {model.sectors.map((sector) => (
            <tr key={sector.method} className={sector.emphasis === 'selected' ? 'selected' : 'dim'}>
              <td><span className="method-swatch" style={{ background: sector.color }} />{sector.method}</td>
              <td>{formatPosition(sector.position.x_m, sector.position.y_m, sector.position.z_m)}</td>
              <td>{sector.radiusM.toFixed(2)} m{sector.radiusSource === 'nominal' ? ' (nominal)' : sector.radiusSource === 'posterior' ? ' (2 sd)' : ''}</td>
              <td>{sector.confidence.toFixed(2)}</td>
              <td>{sector.rmsResidualUs === null ? '—' : `${sector.rmsResidualUs.toFixed(0)} µs`}</td>
              <td>{uncertaintyCell(runs, sector.method)}</td>
              <td>{dimensionalityLabel(sector.dimensionality)}</td>
            </tr>
          ))}
          {model.unavailable.map((entry) => (
            <tr key={`unavailable-${entry.method}`} className="unavailable">
              <td>{entry.method}</td>
              <td colSpan={6}>unavailable: {entry.reason}</td>
            </tr>
          ))}
        </tbody>
      </table>
      )}
      {overlaps.length > 0 && (
        <p className="overlay-note">
          Overlapping estimates: {overlaps.map(([a, b]) => `${a} ↔ ${b}`).join(', ')}.
        </p>
      )}
    </section>
  );
}
