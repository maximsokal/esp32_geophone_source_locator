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
  nonManifoldEdgeCount: number;
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

const MIN_TRIANGLES_RETAINED_COMPONENT = 6;
const MIN_TRIANGLES_QUARANTINED_COMPONENT = 3;
const MIN_AREA_ABSOLUTE_SQUARE_METERS = 0.0015;
const MIN_EFFECTIVE_WIDTH_ABSOLUTE_METERS = 0.032;
const MAX_BOUNDARY_RATIO_FOR_SMALL_COMPONENT = 0.86;

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
    if (![...a, ...b, ...c].every(Number.isFinite)) continue;
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
      const uses = edges.get(key) ?? [];
      uses.push({ triangle: index, from, to });
      edges.set(key, uses);
    }
  }
  return edges;
}

function orientAndPartition(
  triangles: readonly Triangle[],
  edges: Map<string, EdgeUse[]>,
): {
  flips: Uint8Array;
  componentByTriangle: Int32Array;
  componentTriangles: number[][];
  orientationRepairCount: number;
  orientationConflictCount: number;
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
  let orientationConflictCount = 0;
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
        const requiredFlip = (relation.sameDirection ? 1 : 0) ^ flips[current];
        if (assigned[relation.neighbour] === 0) {
          assigned[relation.neighbour] = 1;
          flips[relation.neighbour] = requiredFlip;
          componentByTriangle[relation.neighbour] = componentId;
          queue.push(relation.neighbour);
        } else if (flips[relation.neighbour] !== requiredFlip) {
          orientationConflictCount += 1;
        }
      }
    }
    componentTriangles.push(members);
  }

  let orientationRepairCount = 0;
  for (const flip of flips) if (flip !== 0) orientationRepairCount += 1;
  return {
    flips,
    componentByTriangle,
    componentTriangles,
    orientationRepairCount,
    // Each contradictory adjacency is observed from both traversal directions.
    orientationConflictCount: Math.ceil(orientationConflictCount / 2),
  };
}

function componentQuality(
  id: number,
  members: readonly number[],
  triangles: readonly Triangle[],
  componentByTriangle: Int32Array,
  edges: Map<string, EdgeUse[]>,
  voxelSizeMeters: number,
  globalOrientationConflicts: number,
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

  let boundaryEdges = 0;
  let componentEdges = 0;
  let nonManifoldEdgeCount = 0;
  for (const uses of edges.values()) {
    const usesInComponent = uses.filter((use) => componentByTriangle[use.triangle] === id).length;
    if (usesInComponent === 0) continue;
    componentEdges += 1;
    if (usesInComponent === 1) boundaryEdges += 1;
    if (usesInComponent > 2) nonManifoldEdgeCount += 1;
  }
  const boundaryEdgeRatio = componentEdges > 0 ? boundaryEdges / componentEdges : 1;

  const safeVoxel = clamp(Math.abs(voxelSizeMeters), 0.01, 0.12);
  const minimumMajor = clamp(safeVoxel * 2.5, 0.085, 0.18);
  const minimumWidth = Math.max(MIN_EFFECTIVE_WIDTH_ABSOLUTE_METERS, safeVoxel * 0.85);
  const minimumArea = Math.max(
    MIN_AREA_ABSOLUTE_SQUARE_METERS,
    minimumMajor * minimumWidth * 0.45,
  );
  const topologyClean = nonManifoldEdgeCount === 0 && globalOrientationConflicts === 0;
  const retain =
    topologyClean &&
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
    nonManifoldEdgeCount,
    orientationConflictCount: topologyClean ? 0 : globalOrientationConflicts,
    disposition: retain ? 'retain' : quarantine ? 'quarantine' : 'discard',
  };
}

/**
 * Conservative post-triangulation sanity pass.
 *
 * It never changes the physical surfel/occupancy map. It only decides whether a
 * triangle island is trustworthy enough to be shown/consumed as a mesh. Shared
 * manifold edges are used to repair winding consistently. Non-manifold or
 * non-orientable topology is quarantined instead of being rendered as a strong
 * surface. Small disconnected islands are discarded from the mesh preview while
 * their original physical points remain available to later rescans.
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
      components: [],
    };
  }

  const quantizationMeters = clamp(Math.abs(voxelSizeMeters) * 0.02, 1e-5, 8e-4);
  const triangles = parseTriangles(trianglePositions, quantizationMeters);
  const edges = buildEdgeUses(triangles);
  const orientation = orientAndPartition(triangles, edges);
  const nonManifoldEdgeCount = [...edges.values()].filter((uses) => uses.length > 2).length;
  const components = orientation.componentTriangles.map((members, id) => componentQuality(
    id,
    members,
    triangles,
    orientation.componentByTriangle,
    edges,
    voxelSizeMeters,
    orientation.orientationConflictCount,
  ));

  const output: number[] = [];
  let retainedTriangleCount = 0;
  let quarantinedTriangleCount = 0;
  let discardedTriangleCount = Math.max(0, sourceTriangleCount - triangles.length);
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
    orientationConflictCount: orientation.orientationConflictCount,
    nonManifoldEdgeCount,
    components,
  };
}
