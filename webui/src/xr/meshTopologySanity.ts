import {
  diagnoseMeshNormalIntrusions,
  type MeshNormalIntrusionComponentStats,
} from './meshNormalIntrusion';
import {
  diagnoseMeshSelfIntersections,
  type MeshSelfIntersectionComponentStats,
} from './meshSelfIntersection';

export type MeshComponentDisposition = 'retain' | 'quarantine' | 'discard';

export interface MeshTopologyComponentQuality {
  id: number;
  triangleCount: number;
  surfaceAreaSquareMeters: number;
  majorExtentMeters: number;
  effectiveWidthMeters: number;
  boundaryEdgeRatio: number;
  nonManifoldEdgeCount: number;
  orientationConflictCount: number;
  /** Non-adjacent triangle pairs that geometrically intersect. */
  selfIntersectionPairCount: number;
  /** Fraction of this component's faces touched by at least one such pair. */
  selfIntersectionAffectedTriangleRatio: number;
  /** Fraction of faces whose short normal ray pierces a non-adjacent face. */
  normalIntrusionAffectedTriangleRatio: number;
  /** Combined diagnostic only; a sharp fold is never discarded from this score alone. */
  meshNoiseScore: number;
  disposition: MeshComponentDisposition;
}

export interface MeshTopologySanityResult {
  /** Only retained mesh triangles. Physical surfels are never deleted by this pass. */
  trianglePositions: Float32Array;
  triangleCount: number;
  sourceTriangleCount: number;
  retainedTriangleCount: number;
  quarantinedTriangleCount: number;
  discardedTriangleCount: number;
  orientationRepairCount: number;
  orientationConflictCount: number;
  /** Non-manifold edges detected before local smallest-triangle repair. */
  nonManifoldEdgeCount: number;
  nonManifoldRepairTriangleCount: number;
  selfIntersectionPairCount: number;
  selfIntersectionAffectedTriangleCount: number;
  selfIntersectionExactTestedPairs: number;
  selfIntersectionBroadPhaseCandidatePairs: number;
  selfIntersectionTruncated: boolean;
  selfIntersectionOversizedTriangleCount: number;
  normalIntrusionAffectedTriangleCount: number;
  normalIntrusionExactTestedPairs: number;
  normalIntrusionBroadPhaseCandidatePairs: number;
  normalIntrusionTruncated: boolean;
  normalIntrusionOversizedTriangleCount: number;
  components: readonly MeshTopologyComponentQuality[];
}

interface Triangle {
  originalIndex: number;
  vertices: [Point, Point, Point];
  keys: [string, string, string];
  area: number;
}

type Point = readonly [number, number, number];

interface EdgeUse {
  triangle: number;
  from: string;
  to: string;
}

interface ComponentEdgeStats {
  edgeCount: number;
  boundaryEdgeCount: number;
  nonManifoldEdgeCount: number;
  orientationConflictCount: number;
}

const MIN_TRIANGLES_RETAINED_COMPONENT = 6;
const MIN_TRIANGLES_QUARANTINED_COMPONENT = 3;
const MIN_AREA_ABSOLUTE_SQUARE_METERS = 0.0015;
const MIN_EFFECTIVE_WIDTH_ABSOLUTE_METERS = 0.032;
const MAX_BOUNDARY_RATIO_FOR_SMALL_COMPONENT = 0.86;
// Noise can quarantine a rendered component, but never discard physical evidence.
// 0.60 requires widespread self-intersection or repeated normal intrusions plus
// weak/open topology. A welded sharp fold does not enter either diagnostic.
const MAX_RETAIN_MESH_NOISE_SCORE = 0.60;

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}

function subtract(a: Point, b: Point): Point {
  return [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
}

function cross(a: Point, b: Point): Point {
  return [
    a[1] * b[2] - a[2] * b[1],
    a[2] * b[0] - a[0] * b[2],
    a[0] * b[1] - a[1] * b[0],
  ];
}

function triangleArea(a: Point, b: Point, c: Point): number {
  const normal = cross(subtract(b, a), subtract(c, a));
  return 0.5 * Math.hypot(normal[0], normal[1], normal[2]);
}

function finitePoint(point: Point): boolean {
  return Number.isFinite(point[0]) && Number.isFinite(point[1]) && Number.isFinite(point[2]);
}

function vertexKey(point: Point, quantizationMeters: number): string {
  return `${Math.round(point[0] / quantizationMeters)}:${Math.round(point[1] / quantizationMeters)}:${Math.round(point[2] / quantizationMeters)}`;
}

function edgeKey(a: string, b: string): string {
  return a < b ? `${a}|${b}` : `${b}|${a}`;
}

function parseTriangles(
  positions: Float32Array,
  quantizationMeters: number,
): Triangle[] {
  const count = Math.floor(positions.length / 9);
  const output: Triangle[] = [];
  for (let triangle = 0; triangle < count; triangle += 1) {
    const offset = triangle * 9;
    const a: Point = [positions[offset], positions[offset + 1], positions[offset + 2]];
    const b: Point = [positions[offset + 3], positions[offset + 4], positions[offset + 5]];
    const c: Point = [positions[offset + 6], positions[offset + 7], positions[offset + 8]];
    if (!finitePoint(a) || !finitePoint(b) || !finitePoint(c)) continue;
    const area = triangleArea(a, b, c);
    if (!Number.isFinite(area) || area <= 1e-10) continue;
    output.push({
      originalIndex: triangle,
      vertices: [a, b, c],
      keys: [
        vertexKey(a, quantizationMeters),
        vertexKey(b, quantizationMeters),
        vertexKey(c, quantizationMeters),
      ],
      area,
    });
  }
  return output;
}

function buildEdgeUses(triangles: readonly Triangle[]): Map<string, EdgeUse[]> {
  const edges = new Map<string, EdgeUse[]>();
  for (let index = 0; index < triangles.length; index += 1) {
    const keys = triangles[index].keys;
    for (const [from, to] of [[keys[0], keys[1]], [keys[1], keys[2]], [keys[2], keys[0]]] as const) {
      const key = edgeKey(from, to);
      const uses = edges.get(key);
      if (uses === undefined) edges.set(key, [{ triangle: index, from, to }]);
      else uses.push({ triangle: index, from, to });
    }
  }
  return edges;
}

/**
 * Open3D-style local repair: for an edge touched by >2 faces, keep the two
 * largest adjacent triangles and remove the smaller ambiguous faces. Doing this
 * locally preserves a large healthy component instead of quarantining it because
 * of one tiny overlapping triangle near a noisy scan boundary.
 */
function pruneNonManifoldTriangles(triangles: readonly Triangle[]): {
  triangles: Triangle[];
  detectedEdgeCount: number;
  removedTriangleCount: number;
} {
  const edges = buildEdgeUses(triangles);
  const removed = new Uint8Array(triangles.length);
  let detectedEdgeCount = 0;
  for (const uses of edges.values()) {
    if (uses.length <= 2) continue;
    detectedEdgeCount += 1;
    const ranked = [...uses].sort((left, right) =>
      triangles[right.triangle].area - triangles[left.triangle].area ||
      triangles[left.triangle].originalIndex - triangles[right.triangle].originalIndex
    );
    for (let index = 2; index < ranked.length; index += 1) removed[ranked[index].triangle] = 1;
  }
  let removedTriangleCount = 0;
  const output: Triangle[] = [];
  for (let index = 0; index < triangles.length; index += 1) {
    if (removed[index] !== 0) {
      removedTriangleCount += 1;
      continue;
    }
    output.push(triangles[index]);
  }
  return { triangles: output, detectedEdgeCount, removedTriangleCount };
}

function orientAndPartition(
  triangles: readonly Triangle[],
  edges: Map<string, EdgeUse[]>,
): {
  flips: Uint8Array;
  componentByTriangle: Int32Array;
  componentTriangles: number[][];
  orientationRepairCount: number;
} {
  const flips = new Uint8Array(triangles.length);
  const assigned = new Uint8Array(triangles.length);
  const componentByTriangle = new Int32Array(triangles.length);
  componentByTriangle.fill(-1);
  const adjacency: Array<Array<{ neighbour: number; sameDirection: boolean }>> =
    Array.from({ length: triangles.length }, () => []);

  for (const uses of edges.values()) {
    if (uses.length !== 2) continue;
    const first = uses[0];
    const second = uses[1];
    const sameDirection = first.from === second.from && first.to === second.to;
    adjacency[first.triangle].push({ neighbour: second.triangle, sameDirection });
    adjacency[second.triangle].push({ neighbour: first.triangle, sameDirection });
  }

  const componentTriangles: number[][] = [];
  for (let seed = 0; seed < triangles.length; seed += 1) {
    if (assigned[seed] !== 0) continue;
    const componentId = componentTriangles.length;
    const members: number[] = [];
    const queue = [seed];
    assigned[seed] = 1;
    componentByTriangle[seed] = componentId;
    for (let read = 0; read < queue.length; read += 1) {
      const current = queue[read];
      members.push(current);
      for (const relation of adjacency[current]) {
        const requiredFlip = ((relation.sameDirection ? 1 : 0) ^ flips[current]) as 0 | 1;
        if (assigned[relation.neighbour] === 0) {
          assigned[relation.neighbour] = 1;
          flips[relation.neighbour] = requiredFlip;
          componentByTriangle[relation.neighbour] = componentId;
          queue.push(relation.neighbour);
        }
      }
    }
    componentTriangles.push(members);
  }

  let orientationRepairCount = 0;
  for (const flip of flips) if (flip !== 0) orientationRepairCount += 1;
  return { flips, componentByTriangle, componentTriangles, orientationRepairCount };
}

/** Accumulate all component edge diagnostics in one O(E) pass. */
function buildComponentEdgeStats(
  componentCount: number,
  flips: Uint8Array,
  componentByTriangle: Int32Array,
  edges: Map<string, EdgeUse[]>,
): ComponentEdgeStats[] {
  const stats: ComponentEdgeStats[] = Array.from({ length: componentCount }, () => ({
    edgeCount: 0,
    boundaryEdgeCount: 0,
    nonManifoldEdgeCount: 0,
    orientationConflictCount: 0,
  }));

  for (const uses of edges.values()) {
    if (uses.length === 0) continue;
    const touchedComponents = new Set<number>();
    for (const use of uses) {
      const componentId = componentByTriangle[use.triangle];
      if (componentId >= 0) touchedComponents.add(componentId);
    }
    for (const componentId of touchedComponents) {
      const component = stats[componentId];
      component.edgeCount += 1;
      let usesInComponent = 0;
      for (const use of uses) if (componentByTriangle[use.triangle] === componentId) usesInComponent += 1;
      if (usesInComponent === 1) component.boundaryEdgeCount += 1;
      if (uses.length > 2) component.nonManifoldEdgeCount += 1;
    }

    if (uses.length !== 2) continue;
    const first = uses[0];
    const second = uses[1];
    const componentId = componentByTriangle[first.triangle];
    if (componentId < 0 || componentByTriangle[second.triangle] !== componentId) continue;
    const sameDirection = first.from === second.from && first.to === second.to;
    const effectiveSame = sameDirection !== (flips[first.triangle] !== flips[second.triangle]);
    if (effectiveSame) stats[componentId].orientationConflictCount += 1;
  }
  return stats;
}

function componentQuality(
  id: number,
  members: readonly number[],
  triangles: readonly Triangle[],
  edgeStats: ComponentEdgeStats,
  selfIntersection: MeshSelfIntersectionComponentStats,
  normalIntrusion: MeshNormalIntrusionComponentStats,
  voxelSizeMeters: number,
): MeshTopologyComponentQuality {
  let area = 0;
  let minX = Number.POSITIVE_INFINITY; let minY = Number.POSITIVE_INFINITY; let minZ = Number.POSITIVE_INFINITY;
  let maxX = Number.NEGATIVE_INFINITY; let maxY = Number.NEGATIVE_INFINITY; let maxZ = Number.NEGATIVE_INFINITY;
  for (const index of members) {
    const triangle = triangles[index];
    area += triangle.area;
    for (const point of triangle.vertices) {
      minX = Math.min(minX, point[0]); minY = Math.min(minY, point[1]); minZ = Math.min(minZ, point[2]);
      maxX = Math.max(maxX, point[0]); maxY = Math.max(maxY, point[1]); maxZ = Math.max(maxZ, point[2]);
    }
  }
  const majorExtentMeters = Math.hypot(maxX - minX, maxY - minY, maxZ - minZ);
  const effectiveWidthMeters = area / Math.max(majorExtentMeters, 1e-6);
  const boundaryEdgeRatio = edgeStats.edgeCount > 0 ? edgeStats.boundaryEdgeCount / edgeStats.edgeCount : 1;

  const safeVoxel = clamp(Math.abs(voxelSizeMeters), 0.01, 0.12);
  const minimumMajor = clamp(safeVoxel * 2.5, 0.085, 0.18);
  const minimumWidth = Math.max(MIN_EFFECTIVE_WIDTH_ABSOLUTE_METERS, safeVoxel * 0.85);
  const minimumArea = Math.max(
    MIN_AREA_ABSOLUTE_SQUARE_METERS,
    minimumMajor * minimumWidth * 0.45,
  );

  // Self-intersections and normal-ray intrusions are evidence, not geometric
  // truth. A real thin two-sided surface can trigger normal intrusion and two
  // independently reconstructed faces can meet at a sharp corner without welded
  // vertices. Intrusion contributes only when another weakness is already present.
  const intersectionNoise = clamp(selfIntersection.affectedTriangleRatio / 0.24, 0, 1);
  const normalIntrusionNoise = clamp(normalIntrusion.intrudingTriangleRatio / 0.22, 0, 1);
  const boundaryNoise = clamp((boundaryEdgeRatio - 0.50) / 0.45, 0, 1);
  const hardTopologyNoise = edgeStats.nonManifoldEdgeCount > 0 || edgeStats.orientationConflictCount > 0 ? 1 : 0;
  const intrusionContext = Math.max(boundaryNoise, intersectionNoise * 0.60, hardTopologyNoise);
  const meshNoiseScore = clamp(
    intersectionNoise * 0.62 +
    boundaryNoise * 0.22 +
    hardTopologyNoise * 0.16 +
    normalIntrusionNoise * intrusionContext * 0.24,
    0,
    1,
  );

  const topologyClean = edgeStats.nonManifoldEdgeCount === 0 && edgeStats.orientationConflictCount === 0;
  const retain =
    topologyClean &&
    meshNoiseScore <= MAX_RETAIN_MESH_NOISE_SCORE &&
    members.length >= MIN_TRIANGLES_RETAINED_COMPONENT &&
    area >= minimumArea &&
    majorExtentMeters >= minimumMajor &&
    effectiveWidthMeters >= minimumWidth &&
    (members.length >= 18 || boundaryEdgeRatio <= MAX_BOUNDARY_RATIO_FOR_SMALL_COMPONENT);
  const quarantine =
    !retain &&
    members.length >= MIN_TRIANGLES_QUARANTINED_COMPONENT &&
    area >= minimumArea * 0.30 &&
    majorExtentMeters >= minimumMajor * 0.60;

  return {
    id,
    triangleCount: members.length,
    surfaceAreaSquareMeters: area,
    majorExtentMeters,
    effectiveWidthMeters,
    boundaryEdgeRatio,
    nonManifoldEdgeCount: edgeStats.nonManifoldEdgeCount,
    orientationConflictCount: edgeStats.orientationConflictCount,
    selfIntersectionPairCount: selfIntersection.intersectingPairCount,
    selfIntersectionAffectedTriangleRatio: selfIntersection.affectedTriangleRatio,
    normalIntrusionAffectedTriangleRatio: normalIntrusion.intrudingTriangleRatio,
    meshNoiseScore,
    disposition: retain ? 'retain' : quarantine ? 'quarantine' : 'discard',
  };
}

/**
 * Conservative post-triangulation sanity pass.
 *
 * It never changes the physical surfel/occupancy map. It only decides whether a
 * triangle island is trustworthy enough to be shown/consumed as a mesh. Tiny
 * non-manifold faces are removed locally first, shared manifold edges then repair
 * winding consistently. Self-intersection and short normal intrusions are both
 * bounded spatial diagnostics. High combined noise can quarantine a component
 * from the rendered/consumed mesh, but never discards the underlying physical
 * surfels and never treats one sharp fold or one nearby shell as sufficient proof.
 */
export function sanitizeMeshTopology(
  trianglePositions: Float32Array,
  voxelSizeMeters: number,
): MeshTopologySanityResult {
  const sourceTriangleCount = Math.floor(trianglePositions.length / 9);
  if (sourceTriangleCount === 0) {
    return {
      trianglePositions: new Float32Array(0),
      triangleCount: 0,
      sourceTriangleCount: 0,
      retainedTriangleCount: 0,
      quarantinedTriangleCount: 0,
      discardedTriangleCount: 0,
      orientationRepairCount: 0,
      orientationConflictCount: 0,
      nonManifoldEdgeCount: 0,
      nonManifoldRepairTriangleCount: 0,
      selfIntersectionPairCount: 0,
      selfIntersectionAffectedTriangleCount: 0,
      selfIntersectionExactTestedPairs: 0,
      selfIntersectionBroadPhaseCandidatePairs: 0,
      selfIntersectionTruncated: false,
      selfIntersectionOversizedTriangleCount: 0,
      normalIntrusionAffectedTriangleCount: 0,
      normalIntrusionExactTestedPairs: 0,
      normalIntrusionBroadPhaseCandidatePairs: 0,
      normalIntrusionTruncated: false,
      normalIntrusionOversizedTriangleCount: 0,
      components: [],
    };
  }

  const quantizationMeters = clamp(Math.abs(voxelSizeMeters) * 0.02, 1e-5, 8e-4);
  const parsed = parseTriangles(trianglePositions, quantizationMeters);
  const nonManifoldRepair = pruneNonManifoldTriangles(parsed);
  const triangles = nonManifoldRepair.triangles;
  const edges = buildEdgeUses(triangles);
  const orientation = orientAndPartition(triangles, edges);
  const componentEdgeStats = buildComponentEdgeStats(
    orientation.componentTriangles.length,
    orientation.flips,
    orientation.componentByTriangle,
    edges,
  );
  const selfIntersection = diagnoseMeshSelfIntersections(
    triangles,
    orientation.componentByTriangle,
    orientation.componentTriangles.length,
    voxelSizeMeters,
  );
  const normalIntrusion = diagnoseMeshNormalIntrusions(
    triangles,
    orientation.componentByTriangle,
    orientation.componentTriangles.length,
    voxelSizeMeters,
  );
  const components = orientation.componentTriangles.map((members, id) => componentQuality(
    id,
    members,
    triangles,
    componentEdgeStats[id],
    selfIntersection.componentStats[id],
    normalIntrusion.componentStats[id],
    voxelSizeMeters,
  ));
  const orientationConflictCount = components.reduce((sum, component) => sum + component.orientationConflictCount, 0);

  const output: number[] = [];
  let retainedTriangleCount = 0;
  let quarantinedTriangleCount = 0;
  let discardedTriangleCount =
    Math.max(0, sourceTriangleCount - parsed.length) +
    nonManifoldRepair.removedTriangleCount;
  for (let index = 0; index < triangles.length; index += 1) {
    const component = components[orientation.componentByTriangle[index]];
    if (component.disposition === 'quarantine') {
      quarantinedTriangleCount += 1;
      continue;
    }
    if (component.disposition === 'discard') {
      discardedTriangleCount += 1;
      continue;
    }
    const [a, b, c] = triangles[index].vertices;
    if (orientation.flips[index] !== 0) output.push(...a, ...c, ...b);
    else output.push(...a, ...b, ...c);
    retainedTriangleCount += 1;
  }

  return {
    trianglePositions: Float32Array.from(output),
    triangleCount: retainedTriangleCount,
    sourceTriangleCount,
    retainedTriangleCount,
    quarantinedTriangleCount,
    discardedTriangleCount,
    orientationRepairCount: orientation.orientationRepairCount,
    orientationConflictCount,
    nonManifoldEdgeCount: nonManifoldRepair.detectedEdgeCount,
    nonManifoldRepairTriangleCount: nonManifoldRepair.removedTriangleCount,
    selfIntersectionPairCount: selfIntersection.intersectingPairCount,
    selfIntersectionAffectedTriangleCount: selfIntersection.affectedTriangleCount,
    selfIntersectionExactTestedPairs: selfIntersection.exactTestedPairs,
    selfIntersectionBroadPhaseCandidatePairs: selfIntersection.broadPhaseCandidatePairs,
    selfIntersectionTruncated: selfIntersection.truncated,
    selfIntersectionOversizedTriangleCount: selfIntersection.oversizedTriangleCount,
    normalIntrusionAffectedTriangleCount: normalIntrusion.intrudingTriangleCount,
    normalIntrusionExactTestedPairs: normalIntrusion.exactTestedPairs,
    normalIntrusionBroadPhaseCandidatePairs: normalIntrusion.candidatePairs,
    normalIntrusionTruncated: normalIntrusion.truncated,
    normalIntrusionOversizedTriangleCount: normalIntrusion.oversizedTriangleCount,
    components,
  };
}
