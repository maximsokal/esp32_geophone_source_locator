import {
  findCertifiedGuideCorner,
  findCertifiedGuideEdgeAtPoint,
  getCertifiedGuideRegistrySummary,
} from './certifiedCornerRegistry';
import {
  resolveManualPlacementCandidate,
  type ManualPlacementMode,
} from './manualPlacementSurface';

export type ProjectPoint = readonly [number, number, number];

export interface ManualControlPoint { id: number; position: ProjectPoint; }
export interface ManualProjectionInput {
  u: number; v: number; depthMeters: number;
  projectionMatrix: ArrayLike<number>; worldFromView: ArrayLike<number>;
}
export interface ManualPointPlacementResult {
  point: ManualControlPoint;
  /** Convenience aliases keep add/update call-sites shape-compatible. */
  id: number;
  position: ProjectPoint;
  created: boolean;
  snapped: boolean;
  placementMode: ManualPlacementMode | null;
  placementConfidence: number | null;
}

export class ManualPlacementRejectedError extends Error {
  constructor(message: string) { super(message); this.name = 'ManualPlacementRejectedError'; }
}

interface CertifiedPlacementToken { position: ProjectPoint; confidence: number; }
let certifiedPlacementToken: CertifiedPlacementToken | null = null;

function finite(value: number): boolean { return Number.isFinite(value); }
function clamp01(value: number): number { return Math.min(1, Math.max(0, value)); }
function pointDistance(a: ProjectPoint, b: ProjectPoint): number { return Math.hypot(a[0]-b[0], a[1]-b[1], a[2]-b[2]); }

export function authorizeCertifiedManualPlacement(position: ProjectPoint, confidence: number): void {
  if (!position.every(Number.isFinite) || !Number.isFinite(confidence) || confidence < 0.70) {
    certifiedPlacementToken = null;
    throw new ManualPlacementRejectedError('Certified structural placement is invalid or below confidence threshold.');
  }
  certifiedPlacementToken = { position: [position[0], position[1], position[2]], confidence: Math.min(0.99, Math.max(0, confidence)) };
}

function consumeCertifiedPlacement(position: ProjectPoint): CertifiedPlacementToken | null {
  const token = certifiedPlacementToken; certifiedPlacementToken = null;
  if (token === null) return null;
  return pointDistance(token.position, position) <= 0.008 ? token : null;
}

export function projectDepthSampleToProject(input: ManualProjectionInput): ProjectPoint | null {
  const { projectionMatrix, worldFromView } = input;
  if (projectionMatrix.length < 16 || worldFromView.length < 16) return null;
  const u = clamp01(input.u), v = clamp01(input.v), depthMeters = input.depthMeters;
  if (!finite(depthMeters) || depthMeters <= 0) return null;
  const p00 = Number(projectionMatrix[0]), p11 = Number(projectionMatrix[5]);
  const p20 = Number(projectionMatrix[8]), p21 = Number(projectionMatrix[9]);
  if (!finite(p00) || !finite(p11) || Math.abs(p00) < 1e-6 || Math.abs(p11) < 1e-6) return null;
  const ndcX = u * 2 - 1, ndcY = 1 - v * 2;
  const viewX = ((ndcX + p20) * depthMeters) / p00;
  const viewY = ((ndcY + p21) * depthMeters) / p11;
  const viewZ = -depthMeters;
  const worldX = Number(worldFromView[0]) * viewX + Number(worldFromView[4]) * viewY + Number(worldFromView[8]) * viewZ + Number(worldFromView[12]);
  const worldY = Number(worldFromView[1]) * viewX + Number(worldFromView[5]) * viewY + Number(worldFromView[9]) * viewZ + Number(worldFromView[13]);
  const worldZ = Number(worldFromView[2]) * viewX + Number(worldFromView[6]) * viewY + Number(worldFromView[10]) * viewZ + Number(worldFromView[14]);
  return [worldX, worldY, worldZ].every(finite) ? [worldX, -worldZ, worldY] : null;
}

export function projectPointToXrReference(point: ProjectPoint): ProjectPoint { return [point[0], point[2], -point[1]]; }

function resolvePlacement(position: ProjectPoint): {
  position: ProjectPoint; mode: ManualPlacementMode | null; confidence: number | null; surfaceLocked: boolean;
} {
  const registry = getCertifiedGuideRegistrySummary();
  if (registry.active && registry.hasTargets) {
    const certifiedCorner = findCertifiedGuideCorner(position);
    if (certifiedCorner !== null) {
      return { position: certifiedCorner.position, mode: 'corner', confidence: certifiedCorner.confidence, surfaceLocked: true };
    }
    // A ray-constrained point ON a robust finite edge is also certified. The
    // operator supplied the missing degree of freedom along the two-plane line.
    const certifiedEdge = findCertifiedGuideEdgeAtPoint(position);
    if (certifiedEdge !== null) {
      return { position, mode: 'edge', confidence: certifiedEdge.confidence, surfaceLocked: true };
    }
    throw new ManualPlacementRejectedError(
      'No certified corner/edge at this position. Keep scanning or aim at a validated structural edge.',
    );
  }

  /*
   * A worker can legitimately publish an empty certified registry before any
   * candidate survives the independent certificate. Empty is not a certificate
   * that the whole scene has no edge/corner. In that state fall back to the
   * local stable-surface resolver. It still requires robust multi-plane support
   * and applies its ray-verified FREE-space veto before accepting a placement.
   */
  const resolution = resolveManualPlacementCandidate(position);
  if (resolution === null) {
    if (registry.active) {
      throw new ManualPlacementRejectedError(
        'No physical stable-surface resolver is available for this empty certified registry. Keep scanning.',
      );
    }
    return { position, mode: null, confidence: null, surfaceLocked: false };
  }
  if (!resolution.accepted || resolution.position === null) throw new ManualPlacementRejectedError(resolution.message);
  return { position: resolution.position, mode: resolution.mode, confidence: resolution.confidence, surfaceLocked: true };
}

export class ManualControlPointStore {
  private readonly points = new Map<number, ManualControlPoint>();
  private readonly order: number[] = [];
  private nextId = 1;

  reset(): void { this.points.clear(); this.order.length = 0; this.nextId = 1; certifiedPlacementToken = null; }

  add(position: ProjectPoint): ManualControlPoint {
    const point: ManualControlPoint = { id: this.nextId++, position: [position[0], position[1], position[2]] };
    this.points.set(point.id, point); this.order.push(point.id); return point;
  }

  private updateStored(id: number, position: ProjectPoint): ManualControlPoint | null {
    if (!this.points.has(id)) return null;
    const point: ManualControlPoint = { id, position: [position[0], position[1], position[2]] };
    this.points.set(id, point); return point;
  }

  private placementResult(
    point: ManualControlPoint,
    created: boolean,
    snapped: boolean,
    placementMode: ManualPlacementMode | null,
    placementConfidence: number | null,
  ): ManualPointPlacementResult {
    return {
      point,
      id: point.id,
      position: point.position,
      created,
      snapped,
      placementMode,
      placementConfidence,
    };
  }

  private storeSnapped(
    position: ProjectPoint, snapMeters: number, placementMode: ManualPlacementMode | null,
    placementConfidence: number | null, firstPlacementSnapped: boolean,
  ): ManualPointPlacementResult {
    let nearest: ManualControlPoint | null = null, nearestDistance = Number.POSITIVE_INFINITY;
    for (const point of this.points.values()) {
      const distance = pointDistance(point.position, position);
      if (distance < nearestDistance) { nearest = point; nearestDistance = distance; }
    }
    if (nearest !== null && nearestDistance <= snapMeters) {
      const blended: ProjectPoint = [
        nearest.position[0] * 0.35 + position[0] * 0.65,
        nearest.position[1] * 0.35 + position[1] * 0.65,
        nearest.position[2] * 0.35 + position[2] * 0.65,
      ];
      const updated = this.updateStored(nearest.id, blended) ?? nearest;
      return this.placementResult(updated, false, true, placementMode, placementConfidence);
    }
    const point = this.add(position);
    return this.placementResult(point, true, firstPlacementSnapped, placementMode, placementConfidence);
  }

  addOrSnap(position: ProjectPoint, snapMeters = 0.065): ManualPointPlacementResult {
    const certified = consumeCertifiedPlacement(position);
    if (certified !== null) return this.addCertifiedOrSnap(certified.position, certified.confidence, snapMeters);
    const resolved = resolvePlacement(position);
    return this.storeSnapped(resolved.position, snapMeters, resolved.mode, resolved.confidence, resolved.surfaceLocked);
  }

  addCertifiedOrSnap(position: ProjectPoint, confidence: number, snapMeters = 0.065): ManualPointPlacementResult {
    if (!position.every(Number.isFinite) || !Number.isFinite(confidence) || confidence < 0.70) {
      throw new ManualPlacementRejectedError('Certified structural placement is invalid or below confidence threshold.');
    }
    return this.storeSnapped(position, snapMeters, 'corner', Math.min(0.99, Math.max(0, confidence)), true);
  }

  update(id: number, position: ProjectPoint): ManualControlPoint | null {
    if (!this.points.has(id)) return null;
    const resolved = resolvePlacement(position);
    return this.updateStored(id, resolved.position);
  }

  removeLast(): ManualControlPoint | null {
    const id = this.order.pop(); if (id === undefined) return null;
    const point = this.points.get(id) ?? null; this.points.delete(id); return point;
  }

  get lastId(): number | null { return this.order.length > 0 ? this.order[this.order.length - 1] : null; }
  get size(): number { return this.points.size; }

  snapshot(): readonly ManualControlPoint[] {
    const output: ManualControlPoint[] = [];
    for (const id of this.order) {
      const point = this.points.get(id);
      if (point !== undefined) output.push({ id: point.id, position: [...point.position] as ProjectPoint });
    }
    return output;
  }

  xrReferencePositions(): Float32Array {
    const output = new Float32Array(this.order.length * 3); let write = 0;
    for (const id of this.order) {
      const point = this.points.get(id); if (point === undefined) continue;
      const xr = projectPointToXrReference(point.position);
      output[write++] = xr[0]; output[write++] = xr[1]; output[write++] = xr[2];
    }
    return write === output.length ? output : output.subarray(0, write);
  }
}