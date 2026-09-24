import { Suspense, lazy, useMemo, useState } from 'react';

import type { LocalizationRun, LocalizationSessionGeometry } from '../../api/deviceApi';
import {
  buildLocalizationOverlay,
  overlappingSectors,
  selectOverlayMethod,
} from '../../domain/localizationOverlay';

const LocalizationOverlayScene = lazy(() => import('./LocalizationOverlayScene'));

interface Props {
  runs: LocalizationRun[];
  geometry: LocalizationSessionGeometry | null;
  /** Tests render without WebGL. */
  renderScene?: boolean;
}

function formatPosition(x: number, y: number, z: number): string {
  return `${x.toFixed(2)}, ${y.toFixed(2)}, ${z.toFixed(2)} m`;
}

/**
 * MAX-161: every persisted method of this recording in one scene; the
 * selected method is vivid, the others dim. Selecting never recomputes.
 */
export default function LocalizationOverlay({ runs, geometry, renderScene = true }: Props) {
  const base = useMemo(() => buildLocalizationOverlay(runs, geometry, null), [runs, geometry]);
  const [selected, setSelected] = useState<string | null>(null);
  const model = selected === null ? base : selectOverlayMethod(base, selected);
  const overlaps = overlappingSectors(model);

  if (model.sectors.length === 0 && model.unavailable.length === 0) return null;

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

      {model.sectors.length > 0 && renderScene && (
        <Suspense fallback={<p>Loading 3D view…</p>}>
          <LocalizationOverlayScene model={model} />
        </Suspense>
      )}
      <p className="overlay-note">
        Sensor frame of this recording (MAIN at the origin, metres). The object scan is not drawn under the sectors until it is registered to this frame (AprilTag registration, MAX-145).
        Sector radius = RMS time residual × propagation speed; “nominal” means the method reported no residual.
      </p>

      <table className="overlay-table">
        <thead>
          <tr><th>Method</th><th>Source (x, y, z)</th><th>Radius</th><th>Confidence</th><th>Residual</th><th>Dim.</th></tr>
        </thead>
        <tbody>
          {model.sectors.map((sector) => (
            <tr key={sector.method} className={sector.emphasis === 'selected' ? 'selected' : 'dim'}>
              <td><span className="method-swatch" style={{ background: sector.color }} />{sector.method}</td>
              <td>{formatPosition(sector.position.x_m, sector.position.y_m, sector.position.z_m)}</td>
              <td>{sector.radiusM.toFixed(2)} m{sector.radiusSource === 'nominal' ? ' (nominal)' : ''}</td>
              <td>{sector.confidence.toFixed(2)}</td>
              <td>{sector.rmsResidualUs === null ? '—' : `${sector.rmsResidualUs.toFixed(0)} µs`}</td>
              <td>{sector.dimensionality}</td>
            </tr>
          ))}
          {model.unavailable.map((entry) => (
            <tr key={`unavailable-${entry.method}`} className="unavailable">
              <td>{entry.method}</td>
              <td colSpan={5}>unavailable: {entry.reason}</td>
            </tr>
          ))}
        </tbody>
      </table>
      {overlaps.length > 0 && (
        <p className="overlay-note">
          Overlapping estimates: {overlaps.map(([a, b]) => `${a} ↔ ${b}`).join(', ')}.
        </p>
      )}
    </section>
  );
}
