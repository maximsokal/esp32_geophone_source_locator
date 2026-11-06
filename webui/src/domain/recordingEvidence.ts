import type {
  LocalizationRun,
  LocalizationRunListing,
  ScanArtifactInfo,
} from '../api/deviceApi';
import type { RegistrationArtifact } from '../xr/fiducial/registrationArtifact';

/**
 * MAX-169: what one saved recording carries, from that recording's own
 * frozen evidence only (never the live topology, never another session).
 */

export const UNDETERMINED_UNCERTAINTY_MM = 2147483647;
const REMOTE_PODS = 4;

export type ScanState = 'unknown' | 'absent' | 'available';
export type LocalizationState = 'unknown' | 'not_calculated' | 'calculated' | 'failed';
export type LocalizationSemanticState = 'solved' | 'rejected' | 'unverified';
export type PolicyState = 'unknown' | 'insufficient' | 'planar_2d' | 'capable_3d';

export function localizationSemanticStatus(state: LocalizationState): {
  state: LocalizationSemanticState;
  severity: 'normal' | 'warning' | 'unavailable';
} {
  if (state === 'calculated') return { state: 'solved', severity: 'normal' };
  if (state === 'failed') return { state: 'rejected', severity: 'warning' };
  return { state: 'unverified', severity: 'unavailable' };
}

export interface EvidencePosition {
  pod: number;
  position: { x_m: number; y_m: number; z_m: number | null };
  leveled: boolean;
}

export interface RecordingEvidence {
  geometryAvailable: boolean;
  /** Computed REMOTE pods (1..4) in this recording's frozen geometry. */
  remotes: number[];
  remoteCount: number | null;
  leveledRemotes: number;
  policy: PolicyState;
  positions: EvidencePosition[];
  /** Worst CS residual across pods, metres (null = none reported). */
  worstResidualM: number | null;
  /** Worst determined 1-sigma position uncertainty, metres. */
  worstUncertaintyM: number | null;
  /** Pods whose position the CS mesh could not determine. */
  undeterminedPods: number[];
  geometryComputedAtMs: number | null;
  scan: ScanState;
  scanBytes: number;
  occupancy: ScanState;
  /** True only when this recording has a validated scan-to-sensor registration. */
  scanRegistered: boolean;
  registrationTier: RegistrationArtifact['tier'];
  registrationReason: string | null;
  localization: LocalizationState;
  solvedMethods: string[];
  failedMethods: string[];
  lastCalculatedAtMs: number | null;
  /** What was doubtful at the start of this recording (empty = nothing or an older session). */
  diagnostics: EvidenceDiagnostic[];
}

export type EvidenceDiagnostic =
  | { kind: 'gate_bypassed' }
  | { kind: 'partial_geometry'; pairs: string[] }
  | { kind: 'source_suspicious' }
  | { kind: 'source_unhealthy' };

function diagnosticsOf(listing: LocalizationRunListing | null): EvidenceDiagnostic[] {
  const diagnostic = listing?.diagnostic;
  if (diagnostic === undefined || diagnostic.available !== true) return [];
  const found: EvidenceDiagnostic[] = [];
  if (diagnostic.gate_bypassed === true) found.push({ kind: 'gate_bypassed' });
  if (diagnostic.partial_geometry === true) found.push({ kind: 'partial_geometry', pairs: diagnostic.excluded_pairs ?? [] });
  if (diagnostic.source_suspicious === true) found.push({ kind: 'source_suspicious' });
  if (diagnostic.source_unhealthy === true) found.push({ kind: 'source_unhealthy' });
  return found;
}

function policyFor(remotes: number): PolicyState {
  if (remotes < 3) return 'insufficient';
  return remotes >= 4 ? 'capable_3d' : 'planar_2d';
}

function scanState(info: ScanArtifactInfo | null | undefined): ScanState {
  if (info === null || info === undefined) return 'unknown';
  return info.present ? 'available' : 'absent';
}

function latestRuns(runs: LocalizationRun[]): LocalizationRun[] {
  const latest = new Map<string, LocalizationRun>();
  for (const run of runs) {
    const current = latest.get(run.method_name);
    if (current === undefined || run.computed_at_ms > current.computed_at_ms) latest.set(run.method_name, run);
  }
  return [...latest.values()];
}

export function buildRecordingEvidence(
  listing: LocalizationRunListing | null,
  scan: ScanArtifactInfo | null,
  occupancy: ScanArtifactInfo | null,
  registration: RegistrationArtifact | null = null,
): RecordingEvidence {
  const geometry = listing?.geometry;
  const computed = geometry?.available === true
    ? geometry.pods.filter((pod) => pod.computed && pod.x_mm !== undefined && pod.y_mm !== undefined)
    : [];
  const remotes = computed.filter((pod) => pod.pod >= 1 && pod.pod <= REMOTE_PODS).map((pod) => pod.pod).sort((a, b) => a - b);

  const residuals = computed.map((pod) => pod.residual_mm).filter((v): v is number => v !== undefined && v > 0);
  const uncertainties = computed
    .map((pod) => pod.uncertainty_mm)
    .filter((v): v is number => v !== undefined && v > 0 && v < UNDETERMINED_UNCERTAINTY_MM);
  const computedAt = computed.map((pod) => pod.computed_at_ms).filter((v): v is number => v !== undefined && v > 0);

  const runs = listing === null ? [] : latestRuns(listing.runs);
  const solved = runs.filter((run) => run.solved && run.source_position !== undefined).map((run) => run.method_name).sort();
  const failed = runs.filter((run) => !(run.solved && run.source_position !== undefined)).map((run) => run.method_name).sort();
  const localization: LocalizationState = listing === null
    ? 'unknown'
    : runs.length === 0 ? 'not_calculated' : solved.length > 0 ? 'calculated' : 'failed';

  return {
    geometryAvailable: computed.length > 0,
    remotes,
    remoteCount: geometry?.available === true ? remotes.length : null,
    leveledRemotes: computed.filter((pod) => pod.pod >= 1 && pod.leveled).length,
    policy: geometry?.available === true ? policyFor(remotes.length) : 'unknown',
    positions: computed.map((pod) => ({
      pod: pod.pod,
      position: {
        x_m: (pod.x_mm ?? 0) / 1000,
        y_m: (pod.y_mm ?? 0) / 1000,
        z_m: pod.z_mm === undefined ? null : pod.z_mm / 1000,
      },
      leveled: pod.leveled,
    })),
    worstResidualM: residuals.length === 0 ? null : Math.max(...residuals) / 1000,
    worstUncertaintyM: uncertainties.length === 0 ? null : Math.max(...uncertainties) / 1000,
    undeterminedPods: computed.filter((pod) => pod.uncertainty_mm === UNDETERMINED_UNCERTAINTY_MM).map((pod) => pod.pod),
    geometryComputedAtMs: computedAt.length === 0 ? null : Math.max(...computedAt),
    scan: scanState(scan),
    scanBytes: scan?.present ? scan.bytes : 0,
    occupancy: scanState(occupancy),
    scanRegistered: registration?.valid === true,
    registrationTier: registration?.valid === true ? registration.tier : null,
    registrationReason: registration?.valid === false ? registration.reason : null,
    localization,
    solvedMethods: solved,
    failedMethods: failed,
    lastCalculatedAtMs: runs.length === 0 ? null : Math.max(...runs.map((run) => run.computed_at_ms)),
    diagnostics: diagnosticsOf(listing),
  };
}
