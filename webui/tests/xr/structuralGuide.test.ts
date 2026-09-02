import { describe, expect, it } from 'vitest';

import {
  StructuralGuideTracker,
  detectStructuralGuideGeometry,
  distancePointToSegment,
  matchEdgeToManualPoints,
  type ProjectPoint,
  type StructuralGuideInput,
} from '../../src/xr/structuralGuide';

interface CloudBuilder {
  positions: number[];
  normals: number[];
  confidence: number[];
}

function builder(): CloudBuilder { return { positions: [], normals: [], confidence: [] }; }
function addPoint(cloud: CloudBuilder, p: ProjectPoint, n: ProjectPoint, confidence = 0.94): void {
  cloud.positions.push(...p); cloud.normals.push(...n); cloud.confidence.push(confidence);
}
function addPlaneX(cloud: CloudBuilder, x = 0): void {
  for (let y = -0.22; y <= 0.2201; y += 0.035) for (let z = 0; z <= 0.4201; z += 0.035) addPoint(cloud, [x, y, z], [1, 0, 0]);
}
function addPlaneY(cloud: CloudBuilder, y = 0): void {
  for (let x = -0.22; x <= 0.2201; x += 0.035) for (let z = 0; z <= 0.4201; z += 0.035) addPoint(cloud, [x, y, z], [0, 1, 0]);
}
function addPlaneZ(cloud: CloudBuilder, z = 0): void {
  for (let x = -0.22; x <= 0.2201; x += 0.035) for (let y = -0.22; y <= 0.2201; y += 0.035) addPoint(cloud, [x, y, z], [0, 0, 1]);
}

/**
 * Build two valid 2-D local plane patches while controlling only their samples
 * close enough to support the mathematical intersection line. Far rows keep the
 * plane fit healthy but sit outside the 7.5-cm finite-line support radius.
 */
function addControlledEdgeSupport(
  cloud: CloudBuilder,
  nearZForX: readonly number[],
  nearZForY: readonly number[],
): void {
  for (const z of [0.005, 0.025, 0.045, 0.065, 0.085, 0.095]) {
    for (const y of [0.085, 0.095]) addPoint(cloud, [0, y, z], [1, 0, 0]);
    for (const x of [0.085, 0.095]) addPoint(cloud, [x, 0, z], [0, 1, 0]);
  }
  for (const z of nearZForX) addPoint(cloud, [0, 0.010, z], [1, 0, 0]);
  for (const z of nearZForY) addPoint(cloud, [0.010, 0, z], [0, 1, 0]);
}

function inputFrom(
  cloud: CloudBuilder,
  keyframeCount = 10,
  camera: ProjectPoint = [0.5, 0.5, 0.5],
  freeSpacePositions?: Float32Array,
): StructuralGuideInput {
  return {
    positions: Float32Array.from(cloud.positions),
    normals: Float32Array.from(cloud.normals),
    confidence: Float32Array.from(cloud.confidence),
    keyframeCount,
    voxelSizeMeters: 0.04,
    viewpoint: { position: camera, keyframeId: keyframeCount },
    freeSpacePositions,
    freeSpaceVoxelSizeMeters: freeSpacePositions === undefined ? undefined : 0.18,
  };
}
function shifted(cloud: CloudBuilder, dx: number): CloudBuilder {
  const output = builder();
  for (let index = 0; index < cloud.confidence.length; index += 1) {
    const offset = index * 3;
    addPoint(
      output,
      [cloud.positions[offset] + dx, cloud.positions[offset + 1], cloud.positions[offset + 2]],
      [cloud.normals[offset], cloud.normals[offset + 1], cloud.normals[offset + 2]],
      cloud.confidence[index],
    );
  }
  return output;
}
function occupancyVoxelCenter(point: ProjectPoint, size = 0.18): ProjectPoint {
  return [
    (Math.floor(point[0] / size) + 0.5) * size,
    (Math.floor(point[1] / size) + 0.5) * size,
    (Math.floor(point[2] / size) + 0.5) * size,
  ];
}

describe('structural guide geometry', () => {
  it('does not invent edges on one flat plane', () => {
    const cloud = builder(); addPlaneX(cloud);
    const result = detectStructuralGuideGeometry(inputFrom(cloud));
    expect(result.planarPatches).toBeGreaterThan(5);
    expect(result.edges).toHaveLength(0);
    expect(result.corners).toHaveLength(0);
  });

  it('finds a two-plane edge without inventing a third-plane corner', () => {
    const cloud = builder(); addPlaneX(cloud); addPlaneY(cloud);
    const result = detectStructuralGuideGeometry(inputFrom(cloud));
    expect(result.edges.length).toBeGreaterThan(0);
    expect(result.edges[0].geometryConfidence).toBeGreaterThan(0.48);
    expect(Math.abs(result.edges[0].direction[2])).toBeGreaterThan(0.65);
    expect(result.corners).toHaveLength(0);
  });

  it('rejects two valid planes when the intersection line has too few distributed physical samples', () => {
    const cloud = builder();
    addControlledEdgeSupport(cloud, [0.010, 0.085], [0.010, 0.085]);
    const result = detectStructuralGuideGeometry(inputFrom(cloud));
    expect(result.planarPatches).toBeGreaterThanOrEqual(2);
    expect(result.edges).toHaveLength(0);
    expect(result.corners).toHaveLength(0);
  });

  it('does not let display margins bridge non-overlapping measured edge intervals', () => {
    const cloud = builder();
    // Before the common-span gate, the +/-2.5-cm display margin could bridge
    // this 1-cm physical gap into a plausible-looking ~4-cm edge.
    addControlledEdgeSupport(cloud, [0.005, 0.025, 0.045], [0.055, 0.075, 0.095]);
    const result = detectStructuralGuideGeometry(inputFrom(cloud));
    expect(result.planarPatches).toBeGreaterThanOrEqual(2);
    expect(result.edges).toHaveLength(0);
  });

  it('finds a three-plane corner near the true intersection', () => {
    const cloud = builder(); addPlaneX(cloud); addPlaneY(cloud); addPlaneZ(cloud);
    const result = detectStructuralGuideGeometry(inputFrom(cloud));
    expect(result.corners.length).toBeGreaterThan(0);
    const nearest = [...result.corners].sort((a, b) => Math.hypot(...a.position) - Math.hypot(...b.position))[0];
    expect(Math.hypot(...nearest.position)).toBeLessThan(0.08);
  });

  it('keeps spatial neighbour comparisons below all-pairs growth', () => {
    const cloud = builder(); addPlaneX(cloud); addPlaneY(cloud); addPlaneZ(cloud);
    const distant = shifted(cloud, 1.20);
    cloud.positions.push(...distant.positions);
    cloud.normals.push(...distant.normals);
    cloud.confidence.push(...distant.confidence);
    const result = detectStructuralGuideGeometry(inputFrom(cloud));
    const allPairs = result.planarPatches * (result.planarPatches - 1) / 2;
    expect(result.candidateComparisons).toBeGreaterThan(0);
    expect(result.candidateComparisons).toBeLessThan(allPairs);
  });

  it('rejects a structural corner that occupancy has verified as free air', () => {
    const cloud = builder(); addPlaneX(cloud); addPlaneY(cloud); addPlaneZ(cloud);
    const baseline = detectStructuralGuideGeometry(inputFrom(cloud));
    const corner = [...baseline.corners].sort((a, b) => Math.hypot(...a.position) - Math.hypot(...b.position))[0];
    expect(corner).toBeDefined();
    const contradicted = detectStructuralGuideGeometry(inputFrom(cloud, 10, [0.5, 0.5, 0.5], Float32Array.from(occupancyVoxelCenter(corner.position))));
    expect(contradicted.rejectedByFreeSpace).toBeGreaterThan(0);
    expect(contradicted.corners.some((candidate) => Math.hypot(...candidate.position) < 0.08)).toBe(false);
  });

  it('does not turn a gently curved single surface into a corner', () => {
    const cloud = builder();
    for (let y = -0.30; y <= 0.3001; y += 0.03) for (let z = 0; z <= 0.5001; z += 0.03) {
      const x = 0.015 * Math.sin(y * 8);
      const slope = 0.12 * Math.cos(y * 8);
      const magnitude = Math.hypot(1, slope);
      addPoint(cloud, [x, y, z], [1 / magnitude, -slope / magnitude, 0]);
    }
    const result = detectStructuralGuideGeometry(inputFrom(cloud));
    expect(result.corners).toHaveLength(0);
  });

  it('ignores low-confidence geometry', () => {
    const cloud = builder();
    for (let i = 0; i < 100; i += 1) addPoint(cloud, [0, (i % 10) * 0.02, Math.floor(i / 10) * 0.02], [1, 0, 0], 0.3);
    const result = detectStructuralGuideGeometry(inputFrom(cloud));
    expect(result.analyzedPoints).toBe(100);
    expect(result.planarPatches).toBe(0);
    expect(result.edges).toHaveLength(0);
  });

  it('treats opposite normal signs as the same plane orientation', () => {
    const cloud = builder();
    for (let y = -0.2; y <= 0.2; y += 0.03) for (let z = 0; z <= 0.3; z += 0.03) {
      addPoint(cloud, [0, y, z], [(Math.round(y * 100) % 2 === 0 ? 1 : -1), 0, 0]);
    }
    const result = detectStructuralGuideGeometry(inputFrom(cloud));
    expect(result.planarPatches).toBeGreaterThan(0);
    expect(result.edges).toHaveLength(0);
  });
});

describe('StructuralGuideTracker candidate-specific multi-view evidence', () => {
  it('cannot mature a suggestion by replaying the same viewpoint', () => {
    const cloud = builder(); addPlaneX(cloud); addPlaneY(cloud); addPlaneZ(cloud);
    const tracker = new StructuralGuideTracker();
    for (const keyframe of [10, 14, 18, 22]) {
      expect(tracker.update(inputFrom(cloud, keyframe, [0.5, 0.5, 0.5])).corners).toHaveLength(0);
    }
  });

  it('matures only after the same candidate is seen from a genuinely different viewpoint', () => {
    const cloud = builder(); addPlaneX(cloud); addPlaneY(cloud); addPlaneZ(cloud);
    const tracker = new StructuralGuideTracker();
    expect(tracker.update(inputFrom(cloud, 10, [0.5, 0.5, 0.5])).corners).toHaveLength(0);
    const result = tracker.update(inputFrom(cloud, 14, [0.30, 0.50, 0.55]));
    expect(result.corners.length).toBeGreaterThan(0);
    expect(result.corners[0].observations).toBe(2);
  });

  it('does not use global keyframe advance as a substitute for viewpoint diversity', () => {
    const cloud = builder(); addPlaneX(cloud); addPlaneY(cloud); addPlaneZ(cloud);
    const tracker = new StructuralGuideTracker();
    tracker.update(inputFrom(cloud, 10, [0.5, 0.5, 0.5]));
    expect(tracker.update(inputFrom(cloud, 100, [0.505, 0.5, 0.5])).corners).toHaveLength(0);
  });

  it('keeps a stable candidate id while geometry crosses the old quantized-cell boundary', () => {
    const cloud = builder(); addPlaneX(cloud); addPlaneY(cloud); addPlaneZ(cloud);
    const tracker = new StructuralGuideTracker();
    tracker.update(inputFrom(cloud, 10, [0.5, 0.5, 0.5]));
    const matured = tracker.update(inputFrom(cloud, 14, [0.30, 0.50, 0.55]));
    const id = matured.corners[0]?.id;
    expect(id).toBeDefined();
    const moved = tracker.update(inputFrom(shifted(cloud, 0.07), 18, [0.15, 0.50, 0.55]));
    expect(moved.corners.some((candidate) => candidate.id === id)).toBe(true);
  });

  it('reset discards temporal confidence', () => {
    const cloud = builder(); addPlaneX(cloud); addPlaneY(cloud); addPlaneZ(cloud);
    const tracker = new StructuralGuideTracker();
    tracker.update(inputFrom(cloud, 10, [0.5, 0.5, 0.5]));
    expect(tracker.update(inputFrom(cloud, 14, [0.3, 0.5, 0.55])).corners.length).toBeGreaterThan(0);
    tracker.reset();
    expect(tracker.update(inputFrom(cloud, 30, [0.2, 0.5, 0.6])).corners).toHaveLength(0);
  });
});

describe('edge to operator geometry matching', () => {
  it('matches two manual points near the same detected edge', () => {
    const cloud = builder(); addPlaneX(cloud); addPlaneY(cloud);
    const tracker = new StructuralGuideTracker();
    tracker.update(inputFrom(cloud, 10, [0.5, 0.5, 0.5]));
    const result = tracker.update(inputFrom(cloud, 14, [0.30, 0.50, 0.55]));
    expect(result.edges.length).toBeGreaterThan(0);
    const edge = result.edges[0];
    const points = [{ id: 1, position: edge.start }, { id: 2, position: edge.end }] as const;
    const match = matchEdgeToManualPoints(edge, points, 0.03);
    expect(match?.[0].id).toBe(1);
    expect(match?.[1].id).toBe(2);
  });

  it('rejects unrelated manual points', () => {
    const cloud = builder(); addPlaneX(cloud); addPlaneY(cloud);
    const tracker = new StructuralGuideTracker();
    tracker.update(inputFrom(cloud, 10, [0.5, 0.5, 0.5]));
    const result = tracker.update(inputFrom(cloud, 14, [0.30, 0.50, 0.55]));
    const edge = result.edges[0];
    const match = matchEdgeToManualPoints(edge, [{ id: 1, position: [4, 4, 4] }, { id: 2, position: [5, 5, 5] }], 0.1);
    expect(match).toBeNull();
  });

  it('computes point-to-segment distance without extending the segment', () => {
    expect(distancePointToSegment([2, 0, 0], [0, 0, 0], [1, 0, 0])).toBeCloseTo(1, 6);
    expect(distancePointToSegment([0.5, 0.2, 0], [0, 0, 0], [1, 0, 0])).toBeCloseTo(0.2, 6);
  });
});
