import {
  resolveManualPlacementCandidate,
  type ManualPlacementMode,
} from './manualPlacementSurface';

export type ProjectPoint = readonly [number, number, number];

export interface ManualControlPoint {
  id: number;
  position: ProjectPoint;
}

export interface ManualProjectionInput {
  u: number;
  v: number;
  depthMeters: number;
  projectionMatrix: ArrayLike<number>;
  worldFromView: ArrayLike<number>;
}

export interface ManualPointPlacementResult {
  point: ManualControlPoint;
  created: boolean;
  snapped: boolean;
  placementMode: ManualPlacementMode | null;
  placementConfidence: number | null;
}

export class ManualPlacementRejectedError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'ManualPlacementRejectedError';
  }
}

interface CertifiedPlacementToken {
  position: ProjectPoint;
  confidence: number;
}

let certifiedPlacementToken: CertifiedPlacementToken | null = null;

function finite(value: number): boolean {
  return Number.isFinite(value);
}

function clamp01(value: number): number {
  return Math.min(1, Math.max(0, value));
}

function pointDistance(a: ProjectPoint, b: ProjectPoint): number {
  return Math.hypot(a[0]-b[0], a[1]-b[1], a[2]-b[2]);
}

/**
 * Authorize exactly one already-certified structural placement. The consumer
 * must subsequently submit the same 3D position through addOrSnap(); unrelated
 * depth/tap coordinates cannot consume the token. A later authorization
 * replaces an older unused token, so there is never a queue of bypasses.
 */
export function authorizeCertifiedManualPlacement(position: ProjectPoint, confidence: number): void {
  if (!position.every(Number.isFinite) || !Number.isFinite(confidence) || confidence < 0.70) {
    certifiedPlacementToken = null;
    throw new ManualPlacementRejectedError('Certified structural endpoint is invalid or below confidence threshold.');
  }
  certifiedPlacementToken = {
    position: [position[0], position[1], position[2]],
    confidence: Math.min(0.99, Math.max(0, confidence)),
  };
}

function consumeCertifiedPlacement(position: ProjectPoint): CertifiedPlacementToken | null {
  const token = certifiedPlacementToken;
  certifiedPlacementToken = null;
  if (token === null) return null;
  return pointDistance(token.position, position) <= 0.008 ? token : null;
}

/**
 * Convert one WebXR CPU-depth sample into the project coordinate convention.
 *
 * WebXR reference space uses +Y up and camera-forward -Z. The diploma project
 * uses X/Y as the local horizontal plane and Z as height, therefore the final
 * mapping is [worldX, -worldZ, worldY].
 */
export function projectDepthSampleToProject(input: ManualProjectionInput): ProjectPoint | null {
  const { projectionMatrix, worldFromView } = input;
  if (projectionMatrix.length < 16 || worldFromView.length < 16) return null;

  const u = clamp01(input.u);
  const v = clamp01(input.v);
  const depthMeters = input.depthMeters;
  if (!finite(depthMeters) || depthMeters <= 0) return null;

  const p00 = Number(projectionMatrix[0]);
  const p11 = Number(projectionMatrix[5]);
  const p20 = Number(projectionMatrix[8]);
  const p21 = Number(projectionMatrix[9]);
  if (!finite(p00) || !finite(p11) || Math.abs(p00) < 1e-6 || Math.abs(p11) < 1e-6) return null;

  const ndcX = u * 2 - 1;
  const ndcY = 1 - v * 2;
  const viewX = ((ndcX + p20) * depthMeters) / p00;
  const viewY = ((ndcY + p21) * depthMeters) / p11;
  const viewZ = -depthMeters;

  const worldX = Number(worldFromView[0]) * viewX
    + Number(worldFromView[4]) * viewY
    + Number(worldFromView[8]) * viewZ
    + Number(worldFromView[12]);
  const worldY = Number(worldFromView[1]) * viewX
    + Number(worldFromView[5]) * viewY
    + Number(worldFromView[9]) * viewZ
    + Number(worldFromView[13]);
  const worldZ = Number(worldFromView[2]) * viewX
    + Number(worldFromView[6]) * viewY
    + Number(worldFromView[10]) * viewZ
    + Number(worldFromView[14]);

  if (![worldX, worldY, worldZ].every(finite)) return null;
  return [worldX, -worldZ, worldY];
}

export function projectPointToXrReference(point: ProjectPoint): ProjectPoint {
  return [point[0], point[2], -point[1]];
}

function resolvePlacement(position: ProjectPoint): {
  position: ProjectPoint;
  mode: ManualPlacementMode | null;
  confidence: number | null;
  surfaceLocked: boolean;
} {
  const resolution = resolveManualPlacementCandidate(position);
  if (resolution === null) {
    // Pure geometry/unit consumers can use the store without a running XR map.
    return { position, mode: null, confidence: null, surfaceLocked: false };
  }
  if (!resolution.accepted || resolution.position === null) {
    throw new ManualPlacementRejectedError(resolution.message);
  }
  return {
    position: resolution.position,
    mode: resolution.mode,
    confidence: resolution.confidence,
    surfaceLocked: true,
  };
}

export class ManualControlPointStore {
  private readonly points = new Map<number, ManualControlPoint>();
  private readonly order: number[] = [];
  private nextId = 1;

  reset(): void {
    this.points.clear();
    this.order.length = 0;
    this.nextId = 1;
    certifiedPlacementToken = null;
  }

  add(position: ProjectPoint): ManualControlPoint {
    const point: ManualControlPoint = {
      id: this.nextId++,
      position: [position[0], position[1], position[2]],
    };
    this.points.set(point.id, point);
    this.order.push(point.id);
    return point;
  }

  private updateStored(id: number, position: ProjectPoint): ManualControlPoint | null {
    if (!this.points.has(id)) return null;
    const point: ManualControlPoint = {
      id,
      position: [position[0], position[1], position[2]],
    };
    this.points.set(id, point);
    return point;
  }

  private storeSnapped(
    position: ProjectPoint,
    snapMeters: number,
    placementMode: ManualPlacementMode | null,
    placementConfidence: number | null,
    firstPlacementSnapped: boolean,
  ): ManualPointPlacementResult {
    let nearest: ManualControlPoint | null = null;
    let nearestDistance = Number.POSITIVE_INFINITY;
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
      return {
        point: updated,
        created: false,
        snapped: true,
        placementMode,
        placementConfidence,
      };
    }
    return {
      point: this.add(position),
      created: true,
      snapped: firstPlacementSnapped,
      placementMode,
      placementConfidence,
    };
  }

  /**
   * A phone tap starts from one low-resolution depth texel, but a rescue-search
   * structural pin must never trust that texel directly. When the active XR map
   * has installed a placement resolver, the candidate is first locked to a
   * multi-view reconstructed edge/corner. Unsupported/flying-pixel candidates
   * are rejected instead of creating a pin in empty space.
   *
   * Duplicate taps are still merged after structural locking.
   */
  addOrSnap(position: ProjectPoint, snapMeters = 0.065): ManualPointPlacementResult {
    const certified = consumeCertifiedPlacement(position);
    if (certified !== null) {
      return this.addCertifiedOrSnap(certified.position, certified.confidence, snapMeters);
    }
    const resolved = resolvePlacement(position);
    return this.storeSnapped(
      resolved.position,
      snapMeters,
      resolved.mode,
      resolved.confidence,
      resolved.surfaceLocked,
    );
  }

  /**
   * Store geometry that has already passed a stronger external certificate.
   * This exists for multi-view two-plane endpoints: the worker requires repeated
   * independent viewpoints + verified FREE beyond the finite edge, and the UI
   * immediately re-certifies the same endpoint before calling this method.
   * Running the ordinary three-plane manual resolver a third time would reject a
   * mathematically valid two-plane endpoint simply because it is not a 3-plane
   * corner. This method does NOT accept raw screen/depth candidates.
   */
  addCertifiedOrSnap(
    position: ProjectPoint,
    confidence: number,
    snapMeters = 0.065,
  ): ManualPointPlacementResult {
    if (!position.every(Number.isFinite) || !Number.isFinite(confidence) || confidence < 0.70) {
      throw new ManualPlacementRejectedError('Certified structural endpoint is invalid or below confidence threshold.');
    }
    return this.storeSnapped(
      position,
      snapMeters,
      'corner',
      Math.min(0.99, Math.max(0, confidence)),
      true,
    );
  }

  update(id: number, position: ProjectPoint): ManualControlPoint | null {
    if (!this.points.has(id)) return null;
    const resolved = resolvePlacement(position);
    return this.updateStored(id, resolved.position);
  }

  removeLast(): ManualControlPoint | null {
    const id = this.order.pop();
    if (id === undefined) return null;
    const point = this.points.get(id) ?? null;
    this.points.delete(id);
    return point;
  }

  get lastId(): number | null {
    return this.order.length > 0 ? this.order[this.order.length - 1] : null;
  }

  get size(): number {
    return this.points.size;
  }

  snapshot(): readonly ManualControlPoint[] {
    const output: ManualControlPoint[] = [];
    for (const id of this.order) {
      const point = this.points.get(id);
      if (point !== undefined) {
        output.push({ id: point.id, position: [...point.position] as ProjectPoint });
      }
    }
    return output;
  }

  xrReferencePositions(): Float32Array {
    const output = new Float32Array(this.order.length * 3);
    let write = 0;
    for (const id of this.order) {
      const point = this.points.get(id);
      if (point === undefined) continue;
      const xr = projectPointToXrReference(point.position);
      output[write++] = xr[0];
      output[write++] = xr[1];
      output[write++] = xr[2];
    }
    return write === output.length ? output : output.subarray(0, write);
  }
}
