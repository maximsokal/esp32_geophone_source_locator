import type { Matrix3, TagPoseResult } from './tagPose';
import { tagNodeForId } from './tagBindings.gen';

/**
 * Live (during the AR scan) view of the AprilTags, independent of any saved recording, localization geometry or registration.
 * It never feeds registration: confirmed 3D positions still come from the multi-view observation store and its fusion.
 *
 *   DETECTED_2D    decoded, no usable pose yet (shown at its capture-time image position, provisional)
 *   POSE_ESTIMATED a plausible pose; the tag square is anchored in the WebXR world and re-projected every frame
 *   CONFIRMED_3D   the multi-view fusion confirmed it (set from outside)
 *   LOST           not decoded for `staleMs`; the last anchored square is kept (devices do not move) and drawn dimmed
 */
export type LiveTagStatus = 'DETECTED_2D' | 'POSE_ESTIMATED' | 'CONFIRMED_3D' | 'LOST';
export type Vec3 = readonly [number, number, number];
export type Quad3 = readonly [Vec3, Vec3, Vec3, Vec3];
export type Quad2 = readonly [readonly [number, number], readonly [number, number], readonly [number, number], readonly [number, number]];

export interface LiveTagState {
  tagId: number;
  node: string;
  status: LiveTagStatus;
  /** Status before LOST, restored when the tag is decoded again. */
  statusBeforeLost: Exclude<LiveTagStatus, 'LOST'>;
  lastSeenMs: number;
  detectionCount: number;
  poseCount: number;
  /** Capture-time image corners (u, v normalized, v down), order BL, BR, TR, TL. */
  corners2d: Quad2;
  /** Smoothed tag square in the WebXR local-floor frame, order BL, BR, TR, TL. Null until a pose was accepted. */
  cornersWorld: Quad3 | null;
  distanceM: number | null;
  lastReprojectionError: number | null;
}

export interface LiveTagInput {
  tagId: number;
  corners: Quad2;
  pose: TagPoseResult;
  /** Edge length of the black square in metres. */
  sizeM: number;
  capturedAtMs: number;
}

export interface LiveTagTrackerOptions {
  staleMs?: number;
  /** A pose whose RMS corner reprojection error (normalized image units) is above this is not trusted for anchoring. */
  maxReprojectionErrorNorm?: number;
  minDistanceM?: number;
  maxDistanceM?: number;
  /** Exponential smoothing weight of a new accepted pose (1 = no smoothing). */
  smoothing?: number;
  /** A new pose farther than this from the anchored square is an outlier until it repeats `reanchorAfter` times. */
  jumpM?: number;
  reanchorAfter?: number;
}

const DEFAULTS: Required<LiveTagTrackerOptions> = {
  staleMs: 1500, maxReprojectionErrorNorm: 0.02, minDistanceM: 0.08, maxDistanceM: 4, smoothing: 0.4, jumpM: 0.1, reanchorAfter: 3,
};

/** World corners of a tag square (centre + rotation) in tag-plane order BL, BR, TR, TL. R is row-major, columns = tag x/y axes. */
export function tagWorldCorners(position: Vec3, rotation: Matrix3, sizeM: number): Quad3 {
  const h = sizeM / 2;
  const at = (x: number, y: number): Vec3 => [
    position[0] + rotation[0] * x + rotation[1] * y,
    position[1] + rotation[3] * x + rotation[4] * y,
    position[2] + rotation[6] * x + rotation[7] * y,
  ];
  return [at(-h, -h), at(h, -h), at(h, h), at(-h, h)];
}

/** WebXR local-floor point -> GeoLocator project frame (X = XR X, Y = -XR Z, Z = XR Y). */
export function xrToProject(point: Vec3): Vec3 { return [point[0], -point[2], point[1]]; }

const centre = (quad: Quad3): Vec3 => [0, 1, 2].map((axis) => quad.reduce((sum, corner) => sum + corner[axis], 0) / 4) as unknown as Vec3;
const distance = (a: Vec3, b: Vec3): number => Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);

export class LiveTagTracker {
  private readonly options: Required<LiveTagTrackerOptions>;
  private readonly tags = new Map<number, LiveTagState>();
  private readonly pendingJump = new Map<number, number>();
  private confirmed = new Set<number>();
  outliersRejected = 0;

  constructor(options: LiveTagTrackerOptions = {}) {
    this.options = { ...DEFAULTS, ...options };
  }

  reset(): void {
    this.tags.clear(); this.pendingJump.clear(); this.confirmed = new Set(); this.outliersRejected = 0;
  }

  /** Tag ids the multi-view fusion has confirmed. */
  setConfirmed(ids: readonly number[]): void {
    this.confirmed = new Set(ids);
    for (const state of this.tags.values()) {
      if (state.status === 'LOST') {
        if (this.confirmed.has(state.tagId) && state.cornersWorld !== null) state.statusBeforeLost = 'CONFIRMED_3D';
        continue;
      }
      if (this.confirmed.has(state.tagId) && state.cornersWorld !== null) { state.status = 'CONFIRMED_3D'; state.statusBeforeLost = 'CONFIRMED_3D'; }
    }
  }

  /** Returns true when the tag was seen for the first time in this session. */
  ingest(input: LiveTagInput): boolean {
    const { tagId, corners, pose, sizeM, capturedAtMs } = input;
    let state = this.tags.get(tagId);
    const first = state === undefined;
    if (state === undefined) {
      state = {
        tagId, node: tagNodeForId(tagId), status: 'DETECTED_2D', statusBeforeLost: 'DETECTED_2D', lastSeenMs: capturedAtMs, detectionCount: 0, poseCount: 0,
        corners2d: corners, cornersWorld: null, distanceM: null, lastReprojectionError: null,
      };
      this.tags.set(tagId, state);
    }
    state.lastSeenMs = capturedAtMs;
    state.detectionCount += 1;
    state.corners2d = corners;
    if (pose.ok) {
      state.lastReprojectionError = pose.reprojectionErrorNorm;
      state.distanceM = pose.distanceM;
      const plausible = Array.isArray(pose.rotationScan) && pose.rotationScan.length === 9 && pose.rotationScan.every(Number.isFinite) &&
        pose.reprojectionErrorNorm <= this.options.maxReprojectionErrorNorm &&
        pose.distanceM >= this.options.minDistanceM && pose.distanceM <= this.options.maxDistanceM;
      if (plausible) this.anchor(state, tagWorldCorners(pose.positionScan, pose.rotationScan, sizeM));
    }
    const base: Exclude<LiveTagStatus, 'LOST'> = this.confirmed.has(tagId) && state.cornersWorld !== null ? 'CONFIRMED_3D'
      : state.cornersWorld !== null ? 'POSE_ESTIMATED' : 'DETECTED_2D';
    state.statusBeforeLost = base;
    state.status = base;
    return first;
  }

  private anchor(state: LiveTagState, next: Quad3): void {
    const previous = state.cornersWorld;
    if (previous === null) {
      state.cornersWorld = next; state.poseCount += 1; this.pendingJump.delete(state.tagId); return;
    }
    if (distance(centre(previous), centre(next)) > this.options.jumpM) {
      const repeats = (this.pendingJump.get(state.tagId) ?? 0) + 1;
      if (repeats >= this.options.reanchorAfter) { state.cornersWorld = next; state.poseCount += 1; this.pendingJump.delete(state.tagId); return; }
      this.pendingJump.set(state.tagId, repeats);
      this.outliersRejected += 1;
      return;
    }
    this.pendingJump.delete(state.tagId);
    const w = this.options.smoothing;
    state.cornersWorld = previous.map((corner, index) => [0, 1, 2].map((axis) => corner[axis] * (1 - w) + next[index][axis] * w)) as unknown as Quad3;
    state.poseCount += 1;
  }

  /** Marks tags that have not decoded for `staleMs` as LOST; returns their ids when newly lost. */
  tick(nowMs: number): number[] {
    const lost: number[] = [];
    for (const state of this.tags.values()) {
      if (state.status !== 'LOST' && nowMs - state.lastSeenMs > this.options.staleMs) { state.status = 'LOST'; lost.push(state.tagId); }
    }
    return lost;
  }

  states(): LiveTagState[] {
    return [...this.tags.values()].sort((a, b) => a.tagId - b.tagId);
  }
}

export type OverlayStyle = 'provisional' | 'anchored' | 'confirmed' | 'lost';
export interface OverlayQuad {
  tagId: number;
  node: string;
  style: OverlayStyle;
  /** Viewport-normalized (u, v) of BL, BR, TR, TL. */
  points: [number, number][];
}

export type ProjectToViewport = (projectPoint: Vec3) => { u: number; v: number } | null;

/**
 * Quads to draw this frame. A tag with an anchored square is re-projected with the CURRENT camera, so it stays on the sensor
 * while the phone moves; a tag without a pose is drawn at its capture-time image position (provisional).
 */
export function liveTagOverlayQuads(states: readonly LiveTagState[], project: ProjectToViewport): OverlayQuad[] {
  const quads: OverlayQuad[] = [];
  for (const state of states) {
    let points: [number, number][] | null = null;
    if (state.cornersWorld !== null) {
      const projected = state.cornersWorld.map((corner) => project(xrToProject(corner)));
      if (projected.every((p) => p !== null)) points = projected.map((p) => [p!.u, p!.v] as [number, number]);
    } else {
      points = state.corners2d.map(([u, v]) => [u, v] as [number, number]);
    }
    if (points === null) continue;
    const style: OverlayStyle = state.status === 'LOST' ? 'lost' : state.status === 'CONFIRMED_3D' ? 'confirmed'
      : state.cornersWorld !== null ? 'anchored' : 'provisional';
    quads.push({ tagId: state.tagId, node: state.node, style, points });
  }
  return quads;
}
