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

export class ManualControlPointStore {
  private readonly points = new Map<number, ManualControlPoint>();
  private readonly order: number[] = [];
  private nextId = 1;

  reset(): void {
    this.points.clear();
    this.order.length = 0;
    this.nextId = 1;
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

  /**
   * Manual placement is noisy on a phone. Repeated taps within snapMeters of an
   * existing corner update that corner instead of creating a cloud of duplicate
   * magenta pins.
   */
  addOrSnap(position: ProjectPoint, snapMeters = 0.065): { point: ManualControlPoint; created: boolean; snapped: boolean } {
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
      const updated = this.update(nearest.id, blended) ?? nearest;
      return { point: updated, created: false, snapped: true };
    }
    return { point: this.add(position), created: true, snapped: false };
  }

  update(id: number, position: ProjectPoint): ManualControlPoint | null {
    if (!this.points.has(id)) return null;
    const point: ManualControlPoint = {
      id,
      position: [position[0], position[1], position[2]],
    };
    this.points.set(id, point);
    return point;
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
