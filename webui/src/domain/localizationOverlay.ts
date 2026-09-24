import type {
  LocalizationRun,
  LocalizationRunPosition,
  LocalizationSessionGeometry,
} from '../api/deviceApi';

/**
 * MAX-161: compare localization methods of ONE saved recording in one scene.
 *
 * Pure model: the latest persisted run of every method becomes either a
 * source sector (position + radius) or an "unavailable" entry carrying its
 * rejection reason -- a failed method is never placed at (0,0,0). Selecting a
 * method only changes emphasis; coordinates are the persisted ones and are
 * never recomputed here.
 *
 * Frame: the session's sensor frame (the frozen per-session geometry, MAIN at
 * the origin). The 3D scan is not drawn under the sectors until the scan is
 * registered to that frame (MAX-145), because drawing it unregistered would
 * put the source in the wrong place on the object.
 */

export type OverlayEmphasis = 'selected' | 'dim';

export interface OverlaySector {
  method: string;
  kind: LocalizationRun['kind'];
  position: LocalizationRunPosition;
  /** Visual radius, metres (see radiusSource). */
  radiusM: number;
  /** residual: RMS time residual x propagation speed; nominal: no residual available. */
  radiusSource: 'residual' | 'nominal';
  confidence: number;
  rmsResidualUs: number | null;
  degreesOfFreedom: number | null;
  dimensionality: LocalizationRun['dimensionality'];
  computedAtMs: number;
  color: string;
  emphasis: OverlayEmphasis;
}

export interface OverlayUnavailable {
  method: string;
  reason: string;
  computedAtMs: number;
}

export interface OverlaySensor {
  pod: number;
  label: string;
  position: LocalizationRunPosition;
}

export interface LocalizationOverlayModel {
  sectors: OverlaySector[];
  unavailable: OverlayUnavailable[];
  sensors: OverlaySensor[];
  selectedMethod: string | null;
}

/** Radius when a method reports no residual: visible, explicitly nominal. */
export const NOMINAL_SECTOR_RADIUS_M = 0.25;
const MIN_SECTOR_RADIUS_M = 0.05;
const MAX_SECTOR_RADIUS_M = 50;

const METHOD_COLORS: Record<string, string> = {
  energy_baseline: '#f59e0b',
  gauss_newton_tdoa: '#38bdf8',
  traveltime_inversion: '#a78bfa',
  semblance_scan: '#34d399',
  traveltime_scan_constrained: '#c084fc',
  semblance_scan_constrained: '#6ee7b7',
  tdcas: '#22d3ee',
  tdcas_scan_constrained: '#67e8f9',
  psafuvs: '#fb923c',
  psafuvs_scan_constrained: '#fdba74',
  suea: '#a3e635',
  suea_scan_voids: '#d9f99d',
  joint_fusion: '#f472b6',
};
const FALLBACK_COLORS = ['#fb7185', '#facc15', '#60a5fa', '#4ade80'];

export function methodColor(method: string, index: number): string {
  return METHOD_COLORS[method] ?? FALLBACK_COLORS[index % FALLBACK_COLORS.length];
}

function latestPerMethod(runs: LocalizationRun[]): LocalizationRun[] {
  const latest = new Map<string, LocalizationRun>();
  for (const run of runs) {
    const current = latest.get(run.method_name);
    if (current === undefined || run.computed_at_ms > current.computed_at_ms) {
      latest.set(run.method_name, run);
    }
  }
  return [...latest.values()].sort((a, b) => a.method_name.localeCompare(b.method_name));
}

function finitePosition(position: LocalizationRunPosition | undefined): position is LocalizationRunPosition {
  return position !== undefined &&
    Number.isFinite(position.x_m) && Number.isFinite(position.y_m) && Number.isFinite(position.z_m);
}

export function sectorRadius(run: LocalizationRun): { radiusM: number; source: 'residual' | 'nominal' } {
  const residual = run.rms_residual_us;
  if (residual === undefined || !Number.isFinite(residual) || residual <= 0 ||
      !Number.isFinite(run.propagation_speed_mps) || run.propagation_speed_mps <= 0) {
    return { radiusM: NOMINAL_SECTOR_RADIUS_M, source: 'nominal' };
  }
  const radius = residual * 1e-6 * run.propagation_speed_mps;
  return {
    radiusM: Math.min(MAX_SECTOR_RADIUS_M, Math.max(MIN_SECTOR_RADIUS_M, radius)),
    source: 'residual',
  };
}

function sensorsOf(geometry: LocalizationSessionGeometry | null | undefined): OverlaySensor[] {
  if (geometry === null || geometry === undefined || !geometry.available) return [];
  return geometry.pods
    .filter((pod) => pod.computed && pod.x_mm !== undefined && pod.y_mm !== undefined)
    .map((pod) => ({
      pod: pod.pod,
      label: pod.pod === 0 ? 'MAIN' : `Pod ${pod.pod}`,
      position: { x_m: (pod.x_mm ?? 0) / 1000, y_m: (pod.y_mm ?? 0) / 1000, z_m: (pod.z_mm ?? 0) / 1000 },
    }));
}

export function buildLocalizationOverlay(
  runs: LocalizationRun[],
  geometry: LocalizationSessionGeometry | null | undefined,
  selectedMethod: string | null,
): LocalizationOverlayModel {
  const sectors: OverlaySector[] = [];
  const unavailable: OverlayUnavailable[] = [];

  latestPerMethod(runs).forEach((run, index) => {
    if (!run.solved || !finitePosition(run.source_position)) {
      unavailable.push({
        method: run.method_name,
        reason: run.rejection_reason !== undefined && run.rejection_reason !== ''
          ? run.rejection_reason
          : run.solved ? 'no source position reported' : 'not solved',
        computedAtMs: run.computed_at_ms,
      });
      return;
    }
    const { radiusM, source } = sectorRadius(run);
    sectors.push({
      method: run.method_name,
      kind: run.kind,
      position: { ...run.source_position },
      radiusM,
      radiusSource: source,
      confidence: run.confidence_score,
      rmsResidualUs: run.rms_residual_us ?? null,
      degreesOfFreedom: run.degrees_of_freedom !== undefined && run.degrees_of_freedom >= 0 ? run.degrees_of_freedom : null,
      dimensionality: run.dimensionality,
      computedAtMs: run.computed_at_ms,
      color: methodColor(run.method_name, index),
      emphasis: 'dim',
    });
  });

  const selected = sectors.some((sector) => sector.method === selectedMethod)
    ? selectedMethod
    : sectors[0]?.method ?? null;

  return {
    sectors: sectors.map((sector) => ({ ...sector, emphasis: sector.method === selected ? 'selected' : 'dim' })),
    unavailable,
    sensors: sensorsOf(geometry),
    selectedMethod: selected,
  };
}

/** Change emphasis only: every coordinate and radius stays as persisted. */
export function selectOverlayMethod(model: LocalizationOverlayModel, method: string): LocalizationOverlayModel {
  if (!model.sectors.some((sector) => sector.method === method)) return model;
  return {
    ...model,
    selectedMethod: method,
    sectors: model.sectors.map((sector) => ({ ...sector, emphasis: sector.method === method ? 'selected' : 'dim' })),
  };
}

/** Pairs of methods whose sectors overlap (centre distance < sum of radii). */
export function overlappingSectors(model: LocalizationOverlayModel): Array<[string, string]> {
  const pairs: Array<[string, string]> = [];
  for (let i = 0; i < model.sectors.length; i += 1) {
    for (let j = i + 1; j < model.sectors.length; j += 1) {
      const a = model.sectors[i];
      const b = model.sectors[j];
      const distance = Math.hypot(
        a.position.x_m - b.position.x_m,
        a.position.y_m - b.position.y_m,
        a.position.z_m - b.position.z_m,
      );
      if (distance < a.radiusM + b.radiusM) pairs.push([a.method, b.method]);
    }
  }
  return pairs;
}
