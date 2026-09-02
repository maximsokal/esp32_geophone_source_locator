import { describe, expect, it } from 'vitest';

import {
  StructuralGuideTracker,
  detectStructuralGuideGeometry,
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
function addPlaneXRegion(
  cloud: CloudBuilder,
  x: number,
  yMin: number,
  yMax: number,
  zMin: number,
  zMax: number,
  step = 0.025,
): void {
  for (let y = yMin; y <= yMax + 1e-6; y += step) for (let z = zMin; z <= zMax + 1e-6; z += step) {
    addPoint(cloud, [x, y, z], [1, 0, 0]);
  }
}
function addPlaneYRegion(
  cloud: CloudBuilder,
  y: number,
  xMin: number,
  xMax: number,
  zMin: number,
  zMax: number,
  step = 0.025,
): void {
  for (let x = xMin; x <= xMax + 1e-6; x += step) for (let z = zMin; z <= zMax + 1e-6; z += step) {
    addPoint(cloud, [x, y, z], [0, 1, 0]);
  }
}
function addPlaneZRegion(
  cloud: CloudBuilder,
  z: number,
  xMin: number,
  xMax: number,
  yMin: number,
  yMax: number,
  step = 0.025,
): void {
  for (let x = xMin; x <= xMax + 1e-6; x += step) for (let y = yMin; y <= yMax + 1e-6; y += step) {
    addPoint(cloud, [x, y, z], [0, 0, 1]);
  }
}
function inputFrom(
  cloud: CloudBuilder,
  keyframeCount = 10,
  camera: ProjectPoint = [0.5, 0.5, 0.5],
): StructuralGuideInput {
  return {
    positions: Float32Array.from(cloud.positions),
    normals: Float32Array.from(cloud.normals),
    confidence: Float32Array.from(cloud.confidence),
    keyframeCount,
    voxelSizeMeters: 0.04,
    viewpoint: { position: camera, keyframeId: keyframeCount },
  };
}
function fullEdgeCloud(x = 0, y = 0): CloudBuilder {
  const cloud = builder();
  addPlaneXRegion(cloud, x, y - 0.20, y + 0.20, 0, 0.40, 0.03);
  addPlaneYRegion(cloud, y, x - 0.20, x + 0.20, 0, 0.40, 0.03);
  return cloud;
}

describe('finite local support for structural intersections', () => {
  it('rejects an infinite-plane edge when neither measured patch approaches the mathematical intersection', () => {
    const cloud = builder();
    // The infinite planes x=0 and y=0 intersect on the z axis, and their patch
    // centers are close enough for the historical center-distance gate. But all
    // measured support stays at least 10 cm away from that line.
    addPlaneXRegion(cloud, 0, 0.10, 0.175, 0.05, 0.175);
    addPlaneYRegion(cloud, 0, 0.10, 0.175, 0.05, 0.175);

    const result = detectStructuralGuideGeometry(inputFrom(cloud));
    expect(result.planarPatches).toBeGreaterThan(0);
    expect(result.edges).toHaveLength(0);
    expect(result.corners).toHaveLength(0);
  });

  it('rejects an edge when both planes approach the same line but their measured intervals do not overlap', () => {
    const cloud = builder();
    addPlaneXRegion(cloud, 0, -0.05, 0.05, 0.00, 0.065, 0.02);
    addPlaneYRegion(cloud, 0, -0.05, 0.05, 0.15, 0.215, 0.02);

    const result = detectStructuralGuideGeometry(inputFrom(cloud));
    expect(result.planarPatches).toBeGreaterThan(0);
    expect(result.edges).toHaveLength(0);
  });

  it('clips a real edge to locally measured support instead of manufacturing length from total point count', () => {
    const cloud = builder();
    addPlaneXRegion(cloud, 0, -0.05, 0.05, 0.10, 0.20, 0.02);
    addPlaneYRegion(cloud, 0, -0.05, 0.05, 0.12, 0.18, 0.02);

    const result = detectStructuralGuideGeometry(inputFrom(cloud));
    expect(result.edges.length).toBeGreaterThan(0);
    const edge = result.edges[0];
    const zMin = Math.min(edge.start[2], edge.end[2]);
    const zMax = Math.max(edge.start[2], edge.end[2]);
    expect(zMin).toBeGreaterThanOrEqual(0.08);
    expect(zMax).toBeLessThanOrEqual(0.22);
    expect(Math.abs(edge.end[2] - edge.start[2])).toBeLessThan(0.18);
  });

  it('rejects a three-plane corner whose infinite solution is not locally supported by all three surfaces', () => {
    const cloud = builder();
    addPlaneXRegion(cloud, 0, 0.10, 0.175, 0.10, 0.175);
    addPlaneYRegion(cloud, 0, 0.10, 0.175, 0.10, 0.175);
    addPlaneZRegion(cloud, 0, 0.10, 0.175, 0.10, 0.175);

    const result = detectStructuralGuideGeometry(inputFrom(cloud));
    expect(result.corners).toHaveLength(0);
  });
});

describe('structural candidate association hardening', () => {
  it('does not mature an existing edge track by jumping laterally to a nearby parallel edge', () => {
    const tracker = new StructuralGuideTracker();
    const first = fullEdgeCloud(0, 0);
    const nearbyParallel = fullEdgeCloud(0.07, 0.07);

    expect(tracker.update(inputFrom(first, 10, [0.5, 0.5, 0.5])).edges).toHaveLength(0);
    // Historical midpoint+direction matching could associate these two different
    // physical intersections because their midpoints are only ~9.9 cm apart.
    const second = tracker.update(inputFrom(nearbyParallel, 14, [0.25, 0.50, 0.55]));
    expect(second.edges).toHaveLength(0);
  });

  it('keeps a real edge track across modest along-edge support-window changes', () => {
    const tracker = new StructuralGuideTracker();
    const first = builder();
    addPlaneXRegion(first, 0, -0.08, 0.08, 0.05, 0.28, 0.025);
    addPlaneYRegion(first, 0, -0.08, 0.08, 0.05, 0.28, 0.025);
    const secondCloud = builder();
    addPlaneXRegion(secondCloud, 0, -0.08, 0.08, 0.10, 0.34, 0.025);
    addPlaneYRegion(secondCloud, 0, -0.08, 0.08, 0.10, 0.34, 0.025);

    tracker.update(inputFrom(first, 10, [0.5, 0.5, 0.5]));
    const result = tracker.update(inputFrom(secondCloud, 14, [0.28, 0.5, 0.55]));
    expect(result.edges.length).toBeGreaterThan(0);
    expect(result.edges[0].observations).toBe(2);
  });
});
