import type {
  LocalizationRun,
  LocalizationRunPosition,
  LocalizationSessionGeometry,
} from '../api/deviceApi';
import type { I18nLocale } from '../i18n';
import { translate } from '../i18n';
import { formatNumber, formatPercent } from '../i18n/format';
import { mixHex, uiTheme } from '../theme';

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

/**
 * MAX-148: unconstrained is the plain TDOA/traveltime/etc. solve; scan
 * constrained is the MAX-147 variant that folds in the registered scan's
 * FREE/OCCUPIED occupancy. The backend distinguishes them purely by
 * method_name suffix (see isConstrainedMethod) -- there is no separate flag.
 */
export type ResultVariant = 'unconstrained' | 'scan_constrained';

export interface OverlaySector {
  objectId: string;
  sessionId: string;
  method: string;
  methodVersion: number;
  kind: LocalizationRun['kind'];
  position: LocalizationRunPosition;
  /** Visual radius, metres (see radiusSource). */
  radiusM: number;
  /**
   * residual: RMS time residual x propagation speed; posterior: twice the
   * largest posterior standard deviation (Bayesian runs); nominal: neither
   * available.
   */
  radiusSource: 'residual' | 'posterior' | 'nominal';
  /** Bayesian runs: the credible region as an axis-aligned box. */
  credibleBox: OverlayCredibleBox | null;
  confidence: number;
  rmsResidualUs: number | null;
  degreesOfFreedom: number | null;
  dimensionality: LocalizationRun['dimensionality'];
  computedAtMs: number;
  color: string;
  emphasis: OverlayEmphasis;
  variant: ResultVariant;
  /** MAX-301: the method found the arrivals inconsistent with the velocity range. */
  velocityInconsistent: boolean;
  velocityStatus: LocalizationRun['velocity_status'];
  estimatedPVelocityMps: LocalizationRun['estimated_p_velocity_mps'];
  velocityRange: LocalizationRun['velocity_range'];
}

export function localizationMachineAttributes(sector: OverlaySector) {
  return {
    'data-object-id': sector.objectId,
    'data-session-id': sector.sessionId,
    'data-method-id': sector.method,
    'data-method-version': String(sector.methodVersion),
    'data-computed-at-ms': String(sector.computedAtMs),
    'data-position-x-m': String(sector.position.x_m),
    'data-position-y-m': String(sector.position.y_m),
    'data-position-z-m': String(sector.position.z_m),
    'data-radius-m': String(sector.radiusM),
    'data-uncertainty-x-m': String(sector.credibleBox?.std.x_m ?? ''),
    'data-uncertainty-y-m': String(sector.credibleBox?.std.y_m ?? ''),
    'data-uncertainty-z-m': String(sector.credibleBox?.std.z_m ?? ''),
    'data-estimated-p-velocity-mps': String(sector.estimatedPVelocityMps ?? ''),
    'data-velocity-min-mps': String(sector.velocityRange?.v_min_mps ?? ''),
    'data-velocity-max-mps': String(sector.velocityRange?.v_max_mps ?? ''),
    'data-velocity-status': sector.velocityStatus ?? 'unknown',
  };
}

export interface OverlayCredibleBox {
  level: number;
  center: LocalizationRunPosition;
  /** Edge lengths, metres (0 on an axis the search did not span). */
  size: LocalizationRunPosition;
  std: LocalizationRunPosition;
}

export interface OverlayUnavailable {
  method: string;
  reason: string;
  computedAtMs: number;
  variant: ResultVariant;
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

/*
 * Method colours are built only from the 11 base UI roles: a hue role (accent,
 * good, warning, error, link) in one of three bright tones, plus MUTED/TEXT for
 * the baseline and the joint fusion. Related methods share a hue and differ by
 * tone, so each of them still has its own colour.
 */
type MethodHue = 'accent' | 'good' | 'warning' | 'error' | 'linkMeasuring' | 'muted' | 'text';
type MethodTone = 'base' | 'light' | 'lighter' | 'soft' | 'deep';

const METHOD_STYLES: Record<string, readonly [MethodHue, MethodTone]> = {
  energy_baseline: ['muted', 'base'],
  gauss_newton_tdoa: ['linkMeasuring', 'base'],
  robust_tdoa: ['linkMeasuring', 'light'],
  xcorr_tdoa: ['linkMeasuring', 'soft'],
  traveltime_inversion: ['accent', 'base'],
  traveltime_scan_constrained: ['accent', 'light'],
  tdcas: ['accent', 'soft'],
  tdcas_scan_constrained: ['accent', 'lighter'],
  semblance_scan: ['good', 'base'],
  semblance_scan_constrained: ['good', 'light'],
  semblance_eikonal_voids: ['good', 'soft'],
  psafuvs: ['warning', 'base'],
  psafuvs_scan_constrained: ['warning', 'light'],
  psp_joint: ['warning', 'soft'],
  suea: ['warning', 'lighter'],
  suea_scan_voids: ['muted', 'light'],
  eikonal_fmm: ['error', 'base'],
  eikonal_scan_voids: ['error', 'light'],
  bayesian: ['error', 'soft'],
  bayesian_scan_voids: ['error', 'lighter'],
  joint_fusion: ['text', 'base'],
};

function tone(hue: MethodHue, level: MethodTone): string {
  const base = uiTheme[hue];
  if (level === 'light') return mixHex(base, uiTheme.text, 0.35);
  if (level === 'lighter') return mixHex(base, uiTheme.text, 0.6);
  if (level === 'soft') return mixHex(base, uiTheme.muted, 0.55);
  if (level === 'deep') return mixHex(base, uiTheme.background, 0.15);
  return base;
}

const METHOD_COLORS: Record<string, string> = Object.fromEntries(
  Object.entries(METHOD_STYLES).map(([name, [hue, level]]) => [name, tone(hue, level)]),
);
/** Unknown method names: the same hues in a tone no known method uses, picked by position. */
const FALLBACK_COLORS = (['error', 'warning', 'linkMeasuring', 'good'] as const).map((hue) => tone(hue, 'deep'));

export function methodColor(method: string, index: number): string {
  return METHOD_COLORS[method] ?? FALLBACK_COLORS[index % FALLBACK_COLORS.length];
}

/*
 * MAX-147 landed the scan-occupancy-constrained variants as their own
 * method_name, always the unconstrained base name plus one of these two
 * suffixes (see localization_calculate.c): "_scan_constrained" for the
 * fitted methods (traveltime, semblance, TDCAS, PSAFUVS) and a "_voids"
 * suffix ("_scan_voids" or "semblance_eikonal_voids") for the methods whose
 * scan use is routing around voids rather than a constrained fit (SUEA,
 * eikonal, the joint semblance/eikonal method, Bayesian). There is no
 * separate backend flag -- the name IS the contract.
 */
const CONSTRAINED_SUFFIXES = ['_scan_constrained', '_voids'];

export function isConstrainedMethod(methodName: string): boolean {
  return CONSTRAINED_SUFFIXES.some((suffix) => methodName.endsWith(suffix));
}

export type RunCurrency = 'current' | 'history_previous_scan' | 'history_scan_unknown' | 'unconstrained';

/** Whether a persisted result still belongs to the scan currently being viewed. */
export function classifyRunCurrency(run: LocalizationRun, currentScanId: string | null): RunCurrency {
  if (!isConstrainedMethod(run.method_name)) return 'unconstrained';
  if (typeof run.scan_constraint_applied !== 'boolean') return 'history_scan_unknown';
  return run.scan_constraint_applied === true && currentScanId !== null && currentScanId.trim().length > 0 &&
    run.scan_frame_id === currentScanId
    ? 'current'
    : 'history_previous_scan';
}

export function resultVariant(methodName: string): ResultVariant {
  return isConstrainedMethod(methodName) ? 'scan_constrained' : 'unconstrained';
}

/** True when a sector's Z is not a solved coordinate (MAX-147/148: 3-REMOTE planar fits report a nominal Z, never a real one). */
export function isPlanarSector(sector: Pick<OverlaySector, 'dimensionality'>): boolean {
  return sector.dimensionality === 'planar_2d';
}

export function dimensionalityLabel(dimensionality: LocalizationRun['dimensionality'], locale: I18nLocale = 'en'): string {
  switch (dimensionality) {
    case 'planar_2d': return '2D';
    case 'capable_3d': return '3D';
    default: return translate(locale, 'overlay.insufficient');
  }
}

export interface ToggleAvailability {
  available: boolean;
  /** Stable reason code for the UI; null when available. */
  reason: string | null;
}

/**
 * MAX-148: the unconstrained/scan-constrained toggle. `hasScan` is null
 * while its presence is still being fetched -- disabled either way, but
 * with a distinct explanation from "no scan at all".
 */
export function toggleAvailability(runs: LocalizationRun[], hasScan: boolean | null): ToggleAvailability {
  if (hasScan === null) return { available: false, reason: 'checking_scan' };
  if (!hasScan) return { available: false, reason: 'no_scan' };
  const hasConstrained = runs.some((run) => isConstrainedMethod(run.method_name));
  if (!hasConstrained) return { available: false, reason: 'no_constrained_results' };
  return { available: true, reason: null };
}

/** Keep only one variant's sectors/unavailable entries; re-derives the default selection and emphasis, never coordinates. */
export function filterOverlayByVariant(model: LocalizationOverlayModel, variant: ResultVariant): LocalizationOverlayModel {
  const sectors = model.sectors.filter((sector) => sector.variant === variant);
  const unavailable = model.unavailable.filter((entry) => entry.variant === variant);
  const selected = sectors.some((sector) => sector.method === model.selectedMethod)
    ? model.selectedMethod
    : sectors.find((sector) => !sector.velocityInconsistent)?.method ?? null;
  return {
    ...model,
    sectors: sectors.map((sector) => ({ ...sector, emphasis: sector.method === selected ? 'selected' : 'dim' })),
    unavailable,
    selectedMethod: selected,
  };
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

function finiteUncertainty(run: LocalizationRun): run is LocalizationRun & { uncertainty: NonNullable<LocalizationRun['uncertainty']> } {
  const u = run.uncertainty;
  return u !== undefined && u.credible_level > 0 && u.credible_level < 1 &&
    finitePosition(u.std_m) && finitePosition(u.credible_min) && finitePosition(u.credible_max);
}

/** The credible region of a Bayesian run as a box, or null. */
export function credibleBox(run: LocalizationRun): OverlayCredibleBox | null {
  if (!finiteUncertainty(run)) return null;
  const { credible_min: lo, credible_max: hi } = run.uncertainty;
  return {
    level: run.uncertainty.credible_level,
    center: { x_m: (lo.x_m + hi.x_m) / 2, y_m: (lo.y_m + hi.y_m) / 2, z_m: (lo.z_m + hi.z_m) / 2 },
    size: { x_m: hi.x_m - lo.x_m, y_m: hi.y_m - lo.y_m, z_m: hi.z_m - lo.z_m },
    std: { ...run.uncertainty.std_m },
  };
}

/** Table text: per-axis 1-sd and the credible region's size ('' when none). */
export function uncertaintyText(run: Pick<LocalizationRun, 'uncertainty'>, locale: I18nLocale = 'en'): string {
  const u = run.uncertainty;
  if (u === undefined || !(u.credible_level > 0)) return '';
  const fixed = { minimumFractionDigits: 2, maximumFractionDigits: 2 };
  const sd = `±${formatNumber(locale, u.std_m.x_m, fixed)} / ${formatNumber(locale, u.std_m.y_m, fixed)} / ${formatNumber(locale, u.std_m.z_m, fixed)} m (${translate(locale, 'overlay.one_sd')})`;
  const percent = formatPercent(locale, u.credible_level);
  const size = u.credible_volume_m3 > 0
    ? `${formatNumber(locale, u.credible_volume_m3, u.credible_volume_m3 < 0.01 ? { notation: 'scientific', maximumFractionDigits: 1 } : fixed)} m³`
    : `${formatNumber(locale, u.credible_area_m2, u.credible_area_m2 < 0.01 ? { notation: 'scientific', maximumFractionDigits: 1 } : fixed)} m²`;
  const inflation = u.sigma_inflation !== undefined && u.sigma_inflation > 1
    ? ` · ${translate(locale, 'overlay.widened_misfit', { factor: formatNumber(locale, u.sigma_inflation, { minimumFractionDigits: 1, maximumFractionDigits: 1 }) })}`
    : '';
  return `${sd} · ${translate(locale, 'overlay.region_percent', { percent })} ${size}${inflation}`;
}

export function sectorRadius(run: LocalizationRun): { radiusM: number; source: 'residual' | 'posterior' | 'nominal' } {
  if (finiteUncertainty(run)) {
    const sd = Math.max(run.uncertainty.std_m.x_m, run.uncertainty.std_m.y_m, run.uncertainty.std_m.z_m);
    if (sd > 0) {
      return { radiusM: Math.min(MAX_SECTOR_RADIUS_M, Math.max(MIN_SECTOR_RADIUS_M, 2 * sd)), source: 'posterior' };
    }
  }
  const residual = run.rms_residual_us;
  /* MAX-301: with a velocity range the residual converts to distance at the
   * fastest plausible speed (conservative); v3/v4 records keep the scalar. */
  const rangeMax = run.velocity_range?.v_max_mps;
  const speed = rangeMax !== undefined && Number.isFinite(rangeMax) && rangeMax > 0
    ? rangeMax
    : run.propagation_speed_mps;
  if (residual === undefined || !Number.isFinite(residual) || residual <= 0 ||
      !Number.isFinite(speed) || speed <= 0) {
    return { radiusM: NOMINAL_SECTOR_RADIUS_M, source: 'nominal' };
  }
  const radius = residual * 1e-6 * speed;
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
        variant: resultVariant(run.method_name),
      });
      return;
    }
    const { radiusM, source } = sectorRadius(run);
    sectors.push({
      objectId: run.object_id,
      sessionId: run.session_id,
      method: run.method_name,
      methodVersion: run.method_version,
      kind: run.kind,
      position: { ...run.source_position },
      radiusM,
      radiusSource: source,
      credibleBox: credibleBox(run),
      confidence: run.confidence_score,
      rmsResidualUs: run.rms_residual_us ?? null,
      degreesOfFreedom: run.degrees_of_freedom !== undefined && run.degrees_of_freedom >= 0 ? run.degrees_of_freedom : null,
      dimensionality: run.dimensionality,
      computedAtMs: run.computed_at_ms,
      color: methodColor(run.method_name, index),
      emphasis: 'dim',
      variant: resultVariant(run.method_name),
      velocityInconsistent: run.velocity_status === 'inconsistent',
      velocityStatus: run.velocity_status,
      estimatedPVelocityMps: run.estimated_p_velocity_mps,
      velocityRange: run.velocity_range,
    });
  });

  /* MAX-301 decision C: an operator may still pick an inconsistent sector
   * explicitly, but it is never chosen as the default primary result. */
  const selected = sectors.some((sector) => sector.method === selectedMethod)
    ? selectedMethod
    : sectors.find((sector) => !sector.velocityInconsistent)?.method ?? null;

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
