export type MeshNoiseHotspotPoint = readonly [number, number, number];

export interface MeshNoiseHotspotTriangle {
  vertices: readonly [MeshNoiseHotspotPoint, MeshNoiseHotspotPoint, MeshNoiseHotspotPoint];
}

export interface MeshNoiseHotspotResult {
  /** Triangle mask for local mesh-only quarantine. Never applies to physical surfels. */
  quarantineMask: Uint8Array;
  hotspotCount: number;
  quarantinedTriangleCount: number;
}

const MIN_SELF_INTERSECTION_TRIANGLES = 2;
const MIN_NORMAL_INTRUSION_TRIANGLES = 2;
const MIN_DUAL_EVIDENCE_TRIANGLES = 1;
const MIN_SUSPICIOUS_TRIANGLES = 4;
const MIN_COMPONENT_TRIANGLES_FOR_LOCAL_REPAIR = 20;
const MAX_LOCAL_REPAIR_FRACTION = 0.20;
const MAX_LOCAL_REPAIR_TRIANGLES_PER_COMPONENT = 32;
const MAX_HOTSPOT_NEIGHBOURHOOD_TRIANGLES = 96;

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}

function centroid(triangle: MeshNoiseHotspotTriangle): MeshNoiseHotspotPoint {
  const [a, b, c] = triangle.vertices;
  return [
    (a[0] + b[0] + c[0]) / 3,
    (a[1] + b[1] + c[1]) / 3,
    (a[2] + b[2] + c[2]) / 3,
  ];
}

function coordinate(value: number, cellSizeMeters: number): number {
  return Math.floor(value / cellSizeMeters);
}

function key(x: number, y: number, z: number): string {
  return `${x}:${y}:${z}`;
}

interface SpatialTriangle {
  index: number;
  component: number;
  gx: number;
  gy: number;
  gz: number;
  selfHit: boolean;
  normalHit: boolean;
}

/**
 * Find small spatially concentrated regions where two independent mesh diagnostics
 * agree that local triangulation is pathological.
 *
 * Safety rules:
 * - normal intrusion alone is insufficient because real thin two-sided surfaces
 *   can legitimately be close along their normals;
 * - self-intersection alone remains diagnostic evidence but is not enough for this
 *   local deletion path either;
 * - a hotspot requires multiple nearby faces from both diagnostics and at least
 *   one face carrying both signals;
 * - local repair is deliberately limited to a small fraction of a substantial
 *   connected component. Widespread noise must stay on the ordinary component-
 *   quarantine path rather than being silently "repaired" away;
 * - only the triangle mesh mask is returned. Physical surfels, occupancy and
 *   UNKNOWN/FREE evidence are never modified here.
 */
export function detectLocalMeshNoiseHotspots(
  triangles: readonly MeshNoiseHotspotTriangle[],
  componentByTriangle: Int32Array,
  componentCount: number,
  selfIntersectionHits: ArrayLike<number>,
  normalIntrusionHits: ArrayLike<number>,
  voxelSizeMeters: number,
): MeshNoiseHotspotResult {
  const quarantineMask = new Uint8Array(triangles.length);
  if (triangles.length === 0 || componentCount <= 0) {
    return { quarantineMask, hotspotCount: 0, quarantinedTriangleCount: 0 };
  }

  const componentSizes = new Uint32Array(componentCount);
  const cellSizeMeters = clamp(Math.abs(voxelSizeMeters) * 2.5, 0.08, 0.20);
  const buckets = new Map<string, SpatialTriangle[]>();
  const suspiciousCells = new Set<string>();

  for (let index = 0; index < triangles.length; index += 1) {
    const component = index < componentByTriangle.length ? componentByTriangle[index] : -1;
    if (component < 0 || component >= componentCount) continue;
    componentSizes[component] += 1;
    const selfHit = index < selfIntersectionHits.length && Number(selfIntersectionHits[index]) > 0;
    const normalHit = index < normalIntrusionHits.length && Number(normalIntrusionHits[index]) > 0;
    if (!selfHit && !normalHit) continue;
    const center = centroid(triangles[index]);
    const gx = coordinate(center[0], cellSizeMeters);
    const gy = coordinate(center[1], cellSizeMeters);
    const gz = coordinate(center[2], cellSizeMeters);
    const cellKey = key(gx, gy, gz);
    const value: SpatialTriangle = { index, component, gx, gy, gz, selfHit, normalHit };
    const bucket = buckets.get(cellKey);
    if (bucket === undefined) buckets.set(cellKey, [value]);
    else bucket.push(value);
    suspiciousCells.add(cellKey);
  }

  // Build candidates first. Overlapping 3x3x3 neighbourhoods are unioned later,
  // so one noisy patch cannot be counted/removed repeatedly merely because it
  // straddles hash-cell boundaries. Crucially, each seed examines only components
  // that actually occur in those 27 nearby buckets; we never multiply suspicious
  // cells by all disconnected component ids in a noisy scan.
  const candidateByComponent: Array<Set<number>> = Array.from(
    { length: componentCount },
    () => new Set<number>(),
  );
  const qualifyingCellsByComponent: Array<Set<string>> = Array.from(
    { length: componentCount },
    () => new Set<string>(),
  );

  for (const cellKey of suspiciousCells) {
    const seed = buckets.get(cellKey)?.[0];
    if (seed === undefined) continue;
    const localByComponent = new Map<number, Map<number, SpatialTriangle>>();
    for (let dx = -1; dx <= 1; dx += 1) {
      for (let dy = -1; dy <= 1; dy += 1) {
        for (let dz = -1; dz <= 1; dz += 1) {
          const bucket = buckets.get(key(seed.gx + dx, seed.gy + dy, seed.gz + dz));
          if (bucket === undefined) continue;
          for (const triangle of bucket) {
            if (componentSizes[triangle.component] < MIN_COMPONENT_TRIANGLES_FOR_LOCAL_REPAIR) continue;
            let local = localByComponent.get(triangle.component);
            if (local === undefined) {
              local = new Map<number, SpatialTriangle>();
              localByComponent.set(triangle.component, local);
            }
            if (local.size < MAX_HOTSPOT_NEIGHBOURHOOD_TRIANGLES) {
              local.set(triangle.index, triangle);
            }
          }
        }
      }
    }

    for (const [component, local] of localByComponent) {
      if (local.size < MIN_SUSPICIOUS_TRIANGLES) continue;
      let selfCount = 0;
      let normalCount = 0;
      let dualCount = 0;
      for (const triangle of local.values()) {
        if (triangle.selfHit) selfCount += 1;
        if (triangle.normalHit) normalCount += 1;
        if (triangle.selfHit && triangle.normalHit) dualCount += 1;
      }
      if (
        selfCount < MIN_SELF_INTERSECTION_TRIANGLES ||
        normalCount < MIN_NORMAL_INTRUSION_TRIANGLES ||
        dualCount < MIN_DUAL_EVIDENCE_TRIANGLES
      ) continue;
      qualifyingCellsByComponent[component].add(cellKey);
      for (const triangle of local.values()) candidateByComponent[component].add(triangle.index);
    }
  }

  let hotspotCount = 0;
  let quarantinedTriangleCount = 0;
  for (let component = 0; component < componentCount; component += 1) {
    const candidate = candidateByComponent[component];
    if (candidate.size === 0) continue;
    const maximumLocalRepair = Math.min(
      MAX_LOCAL_REPAIR_TRIANGLES_PER_COMPONENT,
      Math.floor(componentSizes[component] * MAX_LOCAL_REPAIR_FRACTION),
    );
    if (
      maximumLocalRepair < MIN_SUSPICIOUS_TRIANGLES ||
      candidate.size > maximumLocalRepair ||
      componentSizes[component] - candidate.size < 6
    ) {
      // Widespread or structurally destructive evidence is not a local hotspot.
      // Leave it untouched for the existing component-level quarantine policy.
      continue;
    }
    hotspotCount += Math.max(1, qualifyingCellsByComponent[component].size);
    for (const index of candidate) {
      if (quarantineMask[index] !== 0) continue;
      quarantineMask[index] = 1;
      quarantinedTriangleCount += 1;
    }
  }

  return { quarantineMask, hotspotCount, quarantinedTriangleCount };
}
