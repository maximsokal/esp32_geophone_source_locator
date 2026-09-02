import {
  classifyDepthNeighbourhood,
  clamp01,
  lowDiscrepancySample2D,
} from './surfelEvidence';

export type OccupancyPoint = readonly [number, number, number];
export type OccupancyState = 'free' | 'occupied' | 'unknown';

export interface OccupancyBounds {
  min: OccupancyPoint;
  max: OccupancyPoint;
}

export interface OccupancyDepthFrameLike {
  width: number;
  height: number;
  getDepthInMeters(u: number, v: number): number;
  projectionMatrix?: ArrayLike<number>;
  transform?: { matrix: ArrayLike<number> };
}

export interface OccupancyViewLike {
  projectionMatrix: ArrayLike<number>;
  transform: { matrix: ArrayLike<number> };
}

export interface OccupancyFrameContext {
  keyframeId: number;
  evidenceKeyframe: boolean;
  frameWeight: number;
  cameraProjectPosition: OccupancyPoint;
  maxDepthMeters: number;
}

export interface OccupancyIntegrationResult {
  integrated: boolean;
  sampledRays: number;
  freeUpdates: number;
  occupiedUpdates: number;
}

export interface OccupancySnapshot {
  /** Effective spacing used for the bounded UNKNOWN display volume. */
  occupancyVoxelSizeMeters: number;
  /** Exact number of stored cells confidently cleared by repeated camera rays. */
  knownFreeVoxelCount: number;
  /** Exact number of stored cells with repeated depth-hit evidence. */
  occupiedSurfaceVoxelCount: number;
  /** Candidate cells sampled inside the measured rubble/object envelope. */
  unknownCandidateVoxelCount: number;
  freeAirPositions: Float32Array;
  occupiedSurfacePositions: Float32Array;
  unknownCandidatePositions: Float32Array;
}

/**
 * Solver-facing FREE evidence. Unlike OccupancySnapshot this is not a display
 * payload and therefore is never clipped by MAX_DISPLAY_POINTS_PER_CLASS.
 */
export interface FreeSpaceEvidenceSnapshot {
  occupancyVoxelSizeMeters: number;
  /** Exact FREE count in the whole sparse map. */
  knownFreeVoxelCount: number;
  /** FREE voxels actually returned after the optional spatial relevance filter. */
  includedFreeVoxelCount: number;
  freeAirPositions: Float32Array;
}

interface OccupancyCell {
  logOdds: number;
  hitKeyframes: number;
  missKeyframes: number;
  lastHitKeyframe: number;
  lastMissKeyframe: number;
}

const DEFAULT_VOXEL_SIZE_METERS = 0.18;
const DEFAULT_MAX_CELLS = 50000;
const RAYS_PER_EVIDENCE_KEYFRAME = 144;
const SAMPLE_MARGIN = 0.035;
const MIN_DEPTH_METERS = 0.18;
const MIN_LOCAL_SCORE = 0.45;
const MAX_RAY_VOXELS = 192;
const MAX_ENUMERATED_VOLUME_CELLS = 120000;
const MAX_DISPLAY_POINTS_PER_CLASS = 12000;

function probabilityToLogOdds(probability: number): number {
  const p = Math.min(0.999999, Math.max(0.000001, probability));
  return Math.log(p / (1 - p));
}

function logOddsToProbability(logOdds: number): number {
  return 1 / (1 + Math.exp(-logOdds));
}

const HIT_LOG_ODDS = probabilityToLogOdds(0.70);
const MISS_LOG_ODDS = probabilityToLogOdds(0.40);
const FREE_THRESHOLD_LOG_ODDS = probabilityToLogOdds(0.30);
const OCCUPIED_THRESHOLD_LOG_ODDS = probabilityToLogOdds(0.72);
const MIN_LOG_ODDS = probabilityToLogOdds(0.12);
const MAX_LOG_ODDS = probabilityToLogOdds(0.97);

function clamp(value: number, min: number, max: number): number {
  return Math.min(max, Math.max(min, value));
}

function finite(value: number): boolean {
  return Number.isFinite(value);
}

function gridCoordinate(value: number, voxelSizeMeters: number): number {
  return Math.floor(value / voxelSizeMeters);
}

function keyFromCoordinates(x: number, y: number, z: number): string {
  return `${x}:${y}:${z}`;
}

function pointKey(point: OccupancyPoint, voxelSizeMeters: number): string {
  return keyFromCoordinates(
    gridCoordinate(point[0], voxelSizeMeters),
    gridCoordinate(point[1], voxelSizeMeters),
    gridCoordinate(point[2], voxelSizeMeters),
  );
}

function voxelCenterFromCoordinates(
  x: number,
  y: number,
  z: number,
  voxelSizeMeters: number,
): OccupancyPoint {
  return [
    (x + 0.5) * voxelSizeMeters,
    (y + 0.5) * voxelSizeMeters,
    (z + 0.5) * voxelSizeMeters,
  ];
}

function voxelCenterFromKey(key: string, voxelSizeMeters: number): OccupancyPoint {
  const [xText, yText, zText] = key.split(':');
  return voxelCenterFromCoordinates(Number(xText), Number(yText), Number(zText), voxelSizeMeters);
}

function safeDepth(depth: OccupancyDepthFrameLike, u: number, v: number): number | null {
  if (u <= 0 || u >= 1 || v <= 0 || v >= 1) return null;
  const value = depth.getDepthInMeters(u, v);
  return finite(value) && value >= MIN_DEPTH_METERS ? value : null;
}

function traceSegmentVoxelKeys(
  origin: OccupancyPoint,
  end: OccupancyPoint,
  voxelSizeMeters: number,
): string[] {
  let x = gridCoordinate(origin[0], voxelSizeMeters);
  let y = gridCoordinate(origin[1], voxelSizeMeters);
  let z = gridCoordinate(origin[2], voxelSizeMeters);
  const endX = gridCoordinate(end[0], voxelSizeMeters);
  const endY = gridCoordinate(end[1], voxelSizeMeters);
  const endZ = gridCoordinate(end[2], voxelSizeMeters);

  const dx = end[0] - origin[0];
  const dy = end[1] - origin[1];
  const dz = end[2] - origin[2];
  const stepX = dx > 0 ? 1 : dx < 0 ? -1 : 0;
  const stepY = dy > 0 ? 1 : dy < 0 ? -1 : 0;
  const stepZ = dz > 0 ? 1 : dz < 0 ? -1 : 0;
  const tDeltaX = stepX === 0 ? Number.POSITIVE_INFINITY : voxelSizeMeters / Math.abs(dx);
  const tDeltaY = stepY === 0 ? Number.POSITIVE_INFINITY : voxelSizeMeters / Math.abs(dy);
  const tDeltaZ = stepZ === 0 ? Number.POSITIVE_INFINITY : voxelSizeMeters / Math.abs(dz);
  const nextBoundary = (coordinate: number, step: number): number =>
    (coordinate + (step > 0 ? 1 : 0)) * voxelSizeMeters;

  let tMaxX = stepX === 0 ? Number.POSITIVE_INFINITY : (nextBoundary(x, stepX) - origin[0]) / dx;
  let tMaxY = stepY === 0 ? Number.POSITIVE_INFINITY : (nextBoundary(y, stepY) - origin[1]) / dy;
  let tMaxZ = stepZ === 0 ? Number.POSITIVE_INFINITY : (nextBoundary(z, stepZ) - origin[2]) / dz;

  const output: string[] = [];
  for (let iteration = 0; iteration < MAX_RAY_VOXELS; iteration += 1) {
    output.push(keyFromCoordinates(x, y, z));
    if (x === endX && y === endY && z === endZ) break;
    const nextT = Math.min(tMaxX, tMaxY, tMaxZ);
    const epsilon = 1e-10;
    if (tMaxX <= nextT + epsilon) { x += stepX; tMaxX += tDeltaX; }
    if (tMaxY <= nextT + epsilon) { y += stepY; tMaxY += tDeltaY; }
    if (tMaxZ <= nextT + epsilon) { z += stepZ; tMaxZ += tDeltaZ; }
  }
  return output;
}

function unprojectDepthSample(
  u: number,
  v: number,
  depthMeters: number,
  projection: ArrayLike<number>,
  worldFromView: ArrayLike<number>,
): OccupancyPoint | null {
  if (projection.length < 16 || worldFromView.length < 16) return null;
  const p00 = Number(projection[0]);
  const p11 = Number(projection[5]);
  const p20 = Number(projection[8]);
  const p21 = Number(projection[9]);
  if (!finite(p00) || !finite(p11) || Math.abs(p00) < 1e-6 || Math.abs(p11) < 1e-6) return null;

  const ndcX = u * 2 - 1;
  const ndcY = 1 - v * 2;
  const viewX = ((ndcX + p20) * depthMeters) / p00;
  const viewY = ((ndcY + p21) * depthMeters) / p11;
  const viewZ = -depthMeters;
  const worldX = Number(worldFromView[0]) * viewX + Number(worldFromView[4]) * viewY + Number(worldFromView[8]) * viewZ + Number(worldFromView[12]);
  const worldY = Number(worldFromView[1]) * viewX + Number(worldFromView[5]) * viewY + Number(worldFromView[9]) * viewZ + Number(worldFromView[13]);
  const worldZ = Number(worldFromView[2]) * viewX + Number(worldFromView[6]) * viewY + Number(worldFromView[10]) * viewZ + Number(worldFromView[14]);
  if (![worldX, worldY, worldZ].every(finite)) return null;
  return [worldX, -worldZ, worldY];
}

function updateMaxWeight(map: Map<string, number>, key: string, weight: number): void {
  const previous = map.get(key);
  if (previous === undefined || weight > previous) map.set(key, weight);
}

function pointInsideExpandedBounds(point: OccupancyPoint, bounds: OccupancyBounds, marginMeters: number): boolean {
  const margin = Math.max(0, Number.isFinite(marginMeters) ? marginMeters : 0);
  return (
    point[0] >= bounds.min[0] - margin && point[0] <= bounds.max[0] + margin &&
    point[1] >= bounds.min[1] - margin && point[1] <= bounds.max[1] + margin &&
    point[2] >= bounds.min[2] - margin && point[2] <= bounds.max[2] + margin
  );
}

export class SparseOccupancyGrid {
  private readonly voxelSizeMeters: number;
  private readonly maxCells: number;
  private readonly cells = new Map<string, OccupancyCell>();
  /** Exact classified-state indices kept in sync when log-odds crosses thresholds. */
  private readonly freeCellKeys = new Set<string>();
  private readonly occupiedCellKeys = new Set<string>();
  private lastIntegratedKeyframe = -1;

  constructor(voxelSizeMeters = DEFAULT_VOXEL_SIZE_METERS, maxCells = DEFAULT_MAX_CELLS) {
    this.voxelSizeMeters = clamp(voxelSizeMeters, 0.08, 0.50);
    this.maxCells = Math.max(1000, Math.floor(maxCells));
  }

  reset(): void {
    this.cells.clear();
    this.freeCellKeys.clear();
    this.occupiedCellKeys.clear();
    this.lastIntegratedKeyframe = -1;
  }

  get voxelSize(): number { return this.voxelSizeMeters; }
  get cellCount(): number { return this.cells.size; }

  classifyPoint(point: OccupancyPoint): OccupancyState {
    return this.classifyCell(this.cells.get(pointKey(point, this.voxelSizeMeters)));
  }

  probabilityAt(point: OccupancyPoint): number {
    const cell = this.cells.get(pointKey(point, this.voxelSizeMeters));
    return cell === undefined ? 0.5 : logOddsToProbability(cell.logOdds);
  }

  integrateDepthFrame(
    depth: OccupancyDepthFrameLike,
    fallbackView: OccupancyViewLike,
    context: OccupancyFrameContext,
  ): OccupancyIntegrationResult {
    if (!context.evidenceKeyframe || context.keyframeId === this.lastIntegratedKeyframe) {
      return { integrated: false, sampledRays: 0, freeUpdates: 0, occupiedUpdates: 0 };
    }
    if (!finite(context.frameWeight) || context.frameWeight <= 0) {
      return { integrated: false, sampledRays: 0, freeUpdates: 0, occupiedUpdates: 0 };
    }

    const projection = depth.projectionMatrix !== undefined && depth.projectionMatrix.length >= 16
      ? depth.projectionMatrix
      : fallbackView.projectionMatrix;
    const worldFromView = depth.transform !== undefined && depth.transform.matrix.length >= 16
      ? depth.transform.matrix
      : fallbackView.transform.matrix;
    if (projection.length < 16 || worldFromView.length < 16) {
      return { integrated: false, sampledRays: 0, freeUpdates: 0, occupiedUpdates: 0 };
    }

    const freeKeys = new Map<string, number>();
    const occupiedKeys = new Map<string, number>();
    let sampledRays = 0;
    const phase = context.keyframeId * 977 + 17;

    for (let sampleIndex = 0; sampleIndex < RAYS_PER_EVIDENCE_KEYFRAME; sampleIndex += 1) {
      const [u, v] = lowDiscrepancySample2D(sampleIndex, SAMPLE_MARGIN, phase);
      const depthMeters = depth.getDepthInMeters(u, v);
      if (!finite(depthMeters) || depthMeters < MIN_DEPTH_METERS || depthMeters > context.maxDepthMeters) continue;

      const du = 1.5 / Math.max(1, depth.width);
      const dv = 1.5 / Math.max(1, depth.height);
      const local = classifyDepthNeighbourhood(
        depthMeters,
        safeDepth(depth, u - du, v),
        safeDepth(depth, u + du, v),
        safeDepth(depth, u, v - dv),
        safeDepth(depth, u, v + dv),
      );
      if (!local.accepted || local.score < MIN_LOCAL_SCORE) continue;

      const endpoint = unprojectDepthSample(u, v, depthMeters, projection, worldFromView);
      if (endpoint === null) continue;
      const origin = context.cameraProjectPosition;
      const dx = endpoint[0] - origin[0];
      const dy = endpoint[1] - origin[1];
      const dz = endpoint[2] - origin[2];
      const distance = Math.hypot(dx, dy, dz);
      if (!finite(distance) || distance < this.voxelSizeMeters * 0.75) continue;

      const weight = clamp01(context.frameWeight) * local.score;
      if (weight < 0.05) continue;
      sampledRays += 1;

      const endpointKey = pointKey(endpoint, this.voxelSizeMeters);
      updateMaxWeight(occupiedKeys, endpointKey, weight);

      // Inverse sensor model: free evidence stops before the measured hit.
      // Crucially, nothing behind the hit is modified. That hidden rubble volume
      // remains UNKNOWN and therefore remains eligible for source localization.
      const terminalBand = Math.max(this.voxelSizeMeters * 0.80, 0.10);
      const freeDistance = Math.max(0, distance - terminalBand);
      if (freeDistance <= this.voxelSizeMeters * 0.25) continue;
      const scale = freeDistance / distance;
      const freeEnd: OccupancyPoint = [
        origin[0] + dx * scale,
        origin[1] + dy * scale,
        origin[2] + dz * scale,
      ];
      for (const key of traceSegmentVoxelKeys(origin, freeEnd, this.voxelSizeMeters)) {
        if (key !== endpointKey) updateMaxWeight(freeKeys, key, weight);
      }
    }

    // One update per voxel per evidence frame. Hundreds of pixels crossing the
    // same voxel are not hundreds of independent observations. Endpoint hits
    // win over free-ray evidence within that frame.
    for (const key of occupiedKeys.keys()) freeKeys.delete(key);
    for (const [key, weight] of freeKeys) this.updateCell(key, false, weight, context.keyframeId);
    for (const [key, weight] of occupiedKeys) this.updateCell(key, true, weight, context.keyframeId);
    this.lastIntegratedKeyframe = context.keyframeId;

    return {
      integrated: true,
      sampledRays,
      freeUpdates: freeKeys.size,
      occupiedUpdates: occupiedKeys.size,
    };
  }

  /**
   * Physical FREE evidence for structural veto. No display cap is applied.
   * Optional bounds only remove voxels that are too far from any geometry this
   * worker invocation can create; they never change occupancy classification.
   */
  freeSpaceEvidenceSnapshot(
    relevantBounds: OccupancyBounds | null = null,
    marginMeters = 0,
  ): FreeSpaceEvidenceSnapshot {
    const positions = new Float32Array(this.freeCellKeys.size * 3);
    let write = 0;
    for (const key of this.freeCellKeys) {
      const point = voxelCenterFromKey(key, this.voxelSizeMeters);
      if (relevantBounds !== null && !pointInsideExpandedBounds(point, relevantBounds, marginMeters)) continue;
      positions[write++] = point[0];
      positions[write++] = point[1];
      positions[write++] = point[2];
    }
    const includedFreeVoxelCount = Math.floor(write / 3);
    return {
      occupancyVoxelSizeMeters: this.voxelSizeMeters,
      knownFreeVoxelCount: this.freeCellKeys.size,
      includedFreeVoxelCount,
      freeAirPositions: write === positions.length ? positions : positions.slice(0, write),
    };
  }

  snapshot(candidateBounds: OccupancyBounds | null): OccupancySnapshot {
    const free: number[] = [];
    const occupied: number[] = [];

    const append = (target: number[], point: OccupancyPoint): void => {
      if (target.length / 3 >= MAX_DISPLAY_POINTS_PER_CLASS) return;
      target.push(point[0], point[1], point[2]);
    };

    // Classified-state sets avoid rescanning and reclassifying every sparse cell
    // whenever the UI or final snapshot needs display points.
    for (const key of this.freeCellKeys) append(free, voxelCenterFromKey(key, this.voxelSizeMeters));
    for (const key of this.occupiedCellKeys) append(occupied, voxelCenterFromKey(key, this.voxelSizeMeters));
    const knownFreeVoxelCount = this.freeCellKeys.size;
    const occupiedSurfaceVoxelCount = this.occupiedCellKeys.size;

    if (candidateBounds === null) {
      return {
        occupancyVoxelSizeMeters: this.voxelSizeMeters,
        knownFreeVoxelCount,
        occupiedSurfaceVoxelCount,
        unknownCandidateVoxelCount: 0,
        freeAirPositions: Float32Array.from(free),
        occupiedSurfacePositions: Float32Array.from(occupied),
        unknownCandidatePositions: new Float32Array(0),
      };
    }

    // Unknown candidate volume is intentionally bounded by the measured object
    // envelope, not by camera positions. Otherwise all exterior air around the
    // operator would become a fake source-search volume.
    const minX = gridCoordinate(candidateBounds.min[0], this.voxelSizeMeters);
    const minY = gridCoordinate(candidateBounds.min[1], this.voxelSizeMeters);
    const minZ = gridCoordinate(candidateBounds.min[2], this.voxelSizeMeters);
    const maxX = gridCoordinate(candidateBounds.max[0], this.voxelSizeMeters);
    const maxY = gridCoordinate(candidateBounds.max[1], this.voxelSizeMeters);
    const maxZ = gridCoordinate(candidateBounds.max[2], this.voxelSizeMeters);
    const nx = Math.max(1, maxX - minX + 1);
    const ny = Math.max(1, maxY - minY + 1);
    const nz = Math.max(1, maxZ - minZ + 1);
    const totalCells = nx * ny * nz;
    const stride = Math.max(1, Math.ceil(Math.cbrt(totalCells / MAX_ENUMERATED_VOLUME_CELLS)));
    const unknown: number[] = [];
    let unknownCandidateVoxelCount = 0;

    for (let x = minX; x <= maxX; x += stride) {
      for (let y = minY; y <= maxY; y += stride) {
        for (let z = minZ; z <= maxZ; z += stride) {
          const state = this.classifyCell(this.cells.get(keyFromCoordinates(x, y, z)));
          if (state !== 'unknown') continue;
          unknownCandidateVoxelCount += 1;
          append(unknown, voxelCenterFromCoordinates(x, y, z, this.voxelSizeMeters));
        }
      }
    }

    return {
      occupancyVoxelSizeMeters: this.voxelSizeMeters * stride,
      knownFreeVoxelCount,
      occupiedSurfaceVoxelCount,
      unknownCandidateVoxelCount,
      freeAirPositions: Float32Array.from(free),
      occupiedSurfacePositions: Float32Array.from(occupied),
      unknownCandidatePositions: Float32Array.from(unknown),
    };
  }

  private classifyCell(cell: OccupancyCell | undefined): OccupancyState {
    if (cell === undefined) return 'unknown';
    if (cell.logOdds <= FREE_THRESHOLD_LOG_ODDS && cell.missKeyframes >= 2) return 'free';
    if (cell.logOdds >= OCCUPIED_THRESHOLD_LOG_ODDS && cell.hitKeyframes >= 2) return 'occupied';
    return 'unknown';
  }

  private syncClassifiedKey(key: string, previous: OccupancyState, next: OccupancyState): void {
    if (previous === next) return;
    if (previous === 'free') this.freeCellKeys.delete(key);
    else if (previous === 'occupied') this.occupiedCellKeys.delete(key);
    if (next === 'free') this.freeCellKeys.add(key);
    else if (next === 'occupied') this.occupiedCellKeys.add(key);
  }

  private updateCell(key: string, occupied: boolean, weight: number, keyframeId: number): void {
    let cell = this.cells.get(key);
    if (cell === undefined) {
      if (this.cells.size >= this.maxCells) return;
      cell = {
        logOdds: 0,
        hitKeyframes: 0,
        missKeyframes: 0,
        lastHitKeyframe: -1,
        lastMissKeyframe: -1,
      };
      this.cells.set(key, cell);
    }

    const previousState = this.classifyCell(cell);
    const boundedWeight = clamp(weight, 0.08, 1);
    if (occupied) {
      if (cell.lastHitKeyframe === keyframeId) return;
      cell.lastHitKeyframe = keyframeId;
      cell.hitKeyframes += 1;
      cell.logOdds = clamp(cell.logOdds + HIT_LOG_ODDS * boundedWeight, MIN_LOG_ODDS, MAX_LOG_ODDS);
    } else {
      if (cell.lastMissKeyframe === keyframeId) return;
      cell.lastMissKeyframe = keyframeId;
      cell.missKeyframes += 1;
      cell.logOdds = clamp(cell.logOdds + MISS_LOG_ODDS * boundedWeight, MIN_LOG_ODDS, MAX_LOG_ODDS);
    }
    this.syncClassifiedKey(key, previousState, this.classifyCell(cell));
  }
}

export const occupancyMathForTests = {
  probabilityToLogOdds,
  logOddsToProbability,
  traceSegmentVoxelKeys,
};
