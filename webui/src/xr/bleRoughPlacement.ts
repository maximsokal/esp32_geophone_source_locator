/**
 * MAX-146: pre-anchor rough node placement from BLE RSSI.
 *
 * RSSI is an auxiliary, probabilistic hint that helps the operator find the
 * nodes before a visual AprilTag/ArUco anchor exists. It never replaces
 * Channel Sounding and never yields precise XYZ:
 *
 *   not_detected -> ble_detected (distance ring around the phone)
 *                -> rough (horizontal blob, uncertainty >= MIN_ROUGH_RADIUS_M)
 *                -> anchored (visual anchor; RSSI kept only as diagnostics).
 *
 * All positions are in the WebXR reference frame of the scan session
 * (+Y up, see docs/spatial-frames.md). Pure module: no browser APIs.
 */

export type BleNodeState = 'not_detected' | 'ble_detected' | 'rough' | 'anchored';

export type Vec3 = readonly [number, number, number];

export interface RssiSample {
  /** CS node id carried in the Ranging Service data (0 = MAIN). */
  nodeId: number;
  rssiDbm: number;
  /** Advertised TX power if present; otherwise the reference below is used. */
  txPowerDbm: number | null;
  /** Phone position in the XR reference frame when the packet arrived. */
  phonePosition: Vec3;
  timestampMs: number;
}

export interface DistanceInterval {
  medianM: number;
  minM: number;
  maxM: number;
}

export interface BleNodeEstimate {
  nodeId: number;
  label: string;
  state: BleNodeState;
  sampleCount: number;
  lastSeenMs: number | null;
  /** 0..1, deliberately capped below 1 for RSSI-only states. */
  confidence: number;
  /** ble_detected: centre is the latest phone position, radius is the ring. */
  centre: Vec3 | null;
  ringM: DistanceInterval | null;
  /** rough/anchored: 1-sigma-ish horizontal radius of the cloud, metres. */
  uncertaintyM: number | null;
}

export interface BleNodeDiagnostics {
  nodeId: number;
  state: BleNodeState;
  sampleCount: number;
  confidence: number;
  rssiHistory: readonly { t: number; dbm: number }[];
}

/* Log-distance path loss. Reference RSSI at 1 m for a 0 dBm nRF54 advertiser
 * seen by a phone is roughly -59 dBm; rubble/indoor exponents range 1.8..4. */
export const RSSI_AT_1M_DBM = -59;
const PATH_LOSS_N_MEDIAN = 2.7;
const PATH_LOSS_N_MIN = 1.8;
const PATH_LOSS_N_MAX = 4.0;
const SHADOWING_DB = 6;
const MAX_RANGE_M = 60;

export const HISTORY_LIMIT = 64;
export const STALE_AFTER_MS = 30_000;
export const MIN_ROUGH_SAMPLES = 6;
/** Horizontal spread of phone positions needed before trilaterating. */
export const MIN_BASELINE_M = 1.5;
/** RSSI never claims better than this. */
export const MIN_ROUGH_RADIUS_M = 1.5;
const MAX_RSSI_CONFIDENCE = 0.4;
const GRID_STEP_M = 0.25;

export function nodeLabel(nodeId: number): string {
  return nodeId === 0 ? 'MAIN' : `REMOTE ${nodeId}`;
}

export function rssiDistanceInterval(rssiDbm: number, txPowerDbm: number | null = null): DistanceInterval | null {
  if (!Number.isFinite(rssiDbm) || rssiDbm >= 0 || rssiDbm < -127) return null;
  const reference = txPowerDbm !== null && Number.isFinite(txPowerDbm) ? txPowerDbm + RSSI_AT_1M_DBM : RSSI_AT_1M_DBM;
  const loss = reference - rssiDbm;
  const at = (n: number, extraDb: number): number => Math.min(MAX_RANGE_M, Math.max(0.1, 10 ** ((loss + extraDb) / (10 * n))));
  return {
    medianM: at(PATH_LOSS_N_MEDIAN, 0),
    /* Short end: steep exponent and a strong-fade shadowing margin. */
    minM: at(PATH_LOSS_N_MAX, -SHADOWING_DB),
    maxM: at(PATH_LOSS_N_MIN, SHADOWING_DB),
  };
}

function horizontalDistance(a: Vec3, b: Vec3): number {
  return Math.hypot(a[0] - b[0], a[2] - b[2]);
}

function median(values: number[]): number {
  const sorted = [...values].sort((a, b) => a - b);
  const mid = sorted.length >> 1;
  return sorted.length % 2 === 1 ? sorted[mid] : (sorted[mid - 1] + sorted[mid]) / 2;
}

interface NodeTrack {
  samples: RssiSample[];
  anchor: Vec3 | null;
}

/* Per-packet fading and the unknown calibration of the reference RSSI. */
const RSSI_SIGMA_DB = 4;
const CALIBRATION_SIGMA_DB = 8;
/* Delta chi^2 for a 2-parameter ~95 % region. */
const ACCEPT_DELTA_CHI2 = 6;

/**
 * Grid search on the horizontal plane (height of the phone track). For each
 * cell the predicted RSSI follows the median path-loss model; a common
 * calibration offset is fitted (with a prior) so only the RSSI *change* along
 * the phone track has to explain direction. The reported cloud covers every
 * cell inside the Delta-chi^2 region, so a weakly constrained walk yields a
 * large blob rather than a false point.
 */
function roughFix(samples: readonly RssiSample[]): { centre: Vec3; radiusM: number } | null {
  const usable = samples.filter((s) => rssiDistanceInterval(s.rssiDbm, s.txPowerDbm) !== null);
  if (usable.length < MIN_ROUGH_SAMPLES) return null;
  const xs = usable.map((s) => s.phonePosition[0]);
  const zs = usable.map((s) => s.phonePosition[2]);
  const height = median(usable.map((s) => s.phonePosition[1]));
  const reach = Math.max(...usable.map((s) => rssiDistanceInterval(s.rssiDbm, s.txPowerDbm)!.medianM)) + 1;
  const minX = Math.min(...xs) - reach, maxX = Math.max(...xs) + reach;
  const minZ = Math.min(...zs) - reach, maxZ = Math.max(...zs) + reach;
  const step = Math.max(GRID_STEP_M, Math.max(maxX - minX, maxZ - minZ) / 120);
  const n = usable.length;
  const shrink = 1 / (n + (RSSI_SIGMA_DB / CALIBRATION_SIGMA_DB) ** 2);
  const cells: { x: number; z: number; chi2: number }[] = [];
  let best = Number.POSITIVE_INFINITY;
  const residuals = new Array<number>(n);
  for (let x = minX; x <= maxX; x += step) {
    for (let z = minZ; z <= maxZ; z += step) {
      let sum = 0;
      usable.forEach((s, i) => {
        const r = Math.max(0.3, Math.hypot(x - s.phonePosition[0], z - s.phonePosition[2]));
        const reference = RSSI_AT_1M_DBM + (s.txPowerDbm ?? 0);
        residuals[i] = s.rssiDbm - (reference - 10 * PATH_LOSS_N_MEDIAN * Math.log10(r));
        sum += residuals[i];
      });
      const offset = sum * shrink;
      let chi2 = (offset / CALIBRATION_SIGMA_DB) ** 2;
      for (const res of residuals) chi2 += ((res - offset) / RSSI_SIGMA_DB) ** 2;
      cells.push({ x, z, chi2 });
      if (chi2 < best) best = chi2;
    }
  }
  const accepted = cells.filter((c) => c.chi2 <= best + ACCEPT_DELTA_CHI2);
  const cx = accepted.reduce((sum, c) => sum + c.x, 0) / accepted.length;
  const cz = accepted.reduce((sum, c) => sum + c.z, 0) / accepted.length;
  const spread = Math.sqrt(accepted.reduce((sum, c) => sum + (c.x - cx) ** 2 + (c.z - cz) ** 2, 0) / accepted.length);
  return { centre: [cx, height, cz], radiusM: Math.max(MIN_ROUGH_RADIUS_M, spread + step) };
}

function baselineM(samples: readonly RssiSample[]): number {
  let widest = 0;
  for (let i = 0; i < samples.length; i += 1) {
    for (let j = i + 1; j < samples.length; j += 1) {
      widest = Math.max(widest, horizontalDistance(samples[i].phonePosition, samples[j].phonePosition));
    }
  }
  return widest;
}

export class BleRoughPlacement {
  private readonly tracks = new Map<number, NodeTrack>();

  constructor(private readonly nodeIds: readonly number[] = [0, 1, 2, 3, 4]) {}

  private track(nodeId: number): NodeTrack | null {
    if (!this.nodeIds.includes(nodeId)) return null;
    let track = this.tracks.get(nodeId);
    if (track === undefined) {
      track = { samples: [], anchor: null };
      this.tracks.set(nodeId, track);
    }
    return track;
  }

  /** A new XR session has a new reference frame: old phone positions are meaningless. */
  reset(): void {
    this.tracks.clear();
  }

  /** Returns false when the sample is rejected (unknown node, bad values). */
  addSample(sample: RssiSample): boolean {
    const track = this.track(sample.nodeId);
    if (track === null || rssiDistanceInterval(sample.rssiDbm, sample.txPowerDbm) === null) return false;
    if (!sample.phonePosition.every(Number.isFinite) || !Number.isFinite(sample.timestampMs)) return false;
    track.samples.push(sample);
    if (track.samples.length > HISTORY_LIMIT) track.samples.splice(0, track.samples.length - HISTORY_LIMIT);
    return true;
  }

  /** A visual anchor replaces the RSSI estimate entirely. */
  setVisualAnchor(nodeId: number, position: Vec3 | null): void {
    const track = this.track(nodeId);
    if (track !== null) track.anchor = position;
  }

  estimate(nodeId: number, nowMs: number): BleNodeEstimate {
    const track = this.tracks.get(nodeId);
    const base: BleNodeEstimate = {
      nodeId, label: nodeLabel(nodeId), state: 'not_detected', sampleCount: track?.samples.length ?? 0,
      lastSeenMs: track?.samples.at(-1)?.timestampMs ?? null, confidence: 0, centre: null, ringM: null, uncertaintyM: null,
    };
    if (track === undefined) return base;
    if (track.anchor !== null) {
      return { ...base, state: 'anchored', confidence: 1, centre: track.anchor, uncertaintyM: null };
    }
    const fresh = track.samples.filter((s) => nowMs - s.timestampMs <= STALE_AFTER_MS);
    if (fresh.length === 0) return base;
    if (fresh.length >= MIN_ROUGH_SAMPLES && baselineM(fresh) >= MIN_BASELINE_M) {
      const fix = roughFix(fresh);
      if (fix !== null) {
        const confidence = Math.min(MAX_RSSI_CONFIDENCE, (MIN_ROUGH_RADIUS_M / fix.radiusM) * MAX_RSSI_CONFIDENCE);
        return { ...base, state: 'rough', confidence, centre: fix.centre, uncertaintyM: fix.radiusM };
      }
    }
    const latest = fresh[fresh.length - 1];
    const recent = fresh.slice(-5);
    const ring = rssiDistanceInterval(median(recent.map((s) => s.rssiDbm)), latest.txPowerDbm);
    return { ...base, state: 'ble_detected', confidence: 0.1, centre: latest.phonePosition, ringM: ring };
  }

  estimates(nowMs: number): BleNodeEstimate[] {
    return this.nodeIds.map((id) => this.estimate(id, nowMs));
  }

  /** Diagnostic evidence only -- never used as sensor geometry. */
  diagnostics(nowMs: number, historyLimit = 32): BleNodeDiagnostics[] {
    return this.nodeIds.map((id) => {
      const estimate = this.estimate(id, nowMs);
      const samples = this.tracks.get(id)?.samples ?? [];
      return {
        nodeId: id, state: estimate.state, sampleCount: samples.length,
        confidence: Number(estimate.confidence.toFixed(3)),
        rssiHistory: samples.slice(-historyLimit).map((s) => ({ t: s.timestampMs, dbm: s.rssiDbm })),
      };
    });
  }
}

export interface OffscreenIndicator {
  visible: boolean;
  /** Screen-space direction of the arrow, radians; 0 = right, PI/2 = up. */
  angleRad: number;
  distanceM: number;
}

function rotateByInverse(q: readonly [number, number, number, number], v: Vec3): Vec3 {
  /* Rotate v by the conjugate of unit quaternion q = (x, y, z, w). */
  const [qx, qy, qz, qw] = q;
  const ix = -qx, iy = -qy, iz = -qz;
  const tx = 2 * (iy * v[2] - iz * v[1]);
  const ty = 2 * (iz * v[0] - ix * v[2]);
  const tz = 2 * (ix * v[1] - iy * v[0]);
  return [
    v[0] + qw * tx + (iy * tz - iz * ty),
    v[1] + qw * ty + (iz * tx - ix * tz),
    v[2] + qw * tz + (ix * ty - iy * tx),
  ];
}

/**
 * Where the target lies relative to the camera. WebXR view space looks down
 * -Z with +Y up. A target is on screen when it is in front and inside the
 * half field of view on both axes; otherwise an edge arrow is shown.
 */
export function offscreenIndicator(
  pose: { position: Vec3; orientation: readonly [number, number, number, number] },
  target: Vec3,
  halfFovXRad = 0.55,
  halfFovYRad = 0.8,
): OffscreenIndicator {
  const world: Vec3 = [target[0] - pose.position[0], target[1] - pose.position[1], target[2] - pose.position[2]];
  const distanceM = Math.hypot(...world);
  const view = rotateByInverse(pose.orientation, world);
  const forward = -view[2];
  const onScreen = forward > 0 &&
    Math.abs(Math.atan2(view[0], forward)) <= halfFovXRad &&
    Math.abs(Math.atan2(view[1], forward)) <= halfFovYRad;
  /* Behind the camera the lateral components still point the right way. */
  const lateral = Math.hypot(view[0], view[1]);
  const angleRad = lateral < 1e-9 ? -Math.PI / 2 : Math.atan2(view[1], view[0]);
  return { visible: onScreen, angleRad, distanceM };
}
