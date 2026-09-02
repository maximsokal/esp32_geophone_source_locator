import { describe, expect, it } from 'vitest';

import {
  detectEdgeEndpointObservations,
  StructuralEdgeEndpointTracker,
  type EdgeEndpointSnapshot,
} from '../../src/xr/structuralEdgeEndpoints';

interface Cloud {
  positions: number[];
  normals: number[];
  confidence: number[];
  residual: number[];
  normalConfidence: number[];
  coverageConfidence: number[];
  ready: number[];
}

function cloud(): Cloud {
  return {
    positions: [],
    normals: [],
    confidence: [],
    residual: [],
    normalConfidence: [],
    coverageConfidence: [],
    ready: [],
  };
}

function add(
  target: Cloud,
  position: readonly [number, number, number],
  normal: readonly [number, number, number],
): void {
  target.positions.push(...position);
  target.normals.push(...normal);
  target.confidence.push(0.91);
  target.residual.push(0.006);
  target.normalConfidence.push(0.94);
  target.coverageConfidence.push(0.90);
  target.ready.push(1);
}

function edgeCloud(zStart = 0.20, zEnd = 0.50, step = 0.025): Cloud {
  const target = cloud();
  for (let z = zStart; z <= zEnd + 0.0001; z += step) {
    for (const y of [0.018, 0.036, 0.054]) add(target, [0, y, z], [1, 0, 0]);
    for (const x of [0.018, 0.036, 0.054]) add(target, [x, 0, z], [0, 1, 0]);
  }
  return target;
}

function bedEdgeCloud(): Cloud { return edgeCloud(); }

function freeBeyondUpperEndpoint(): Float32Array {
  const points: number[] = [];
  const size = 0.05;
  // Several verified-free cells immediately beyond the physical edge endpoint.
  for (const z of [0.525, 0.575, 0.625, 0.675]) {
    points.push(size * 0.5, size * 0.5, z);
  }
  return Float32Array.from(points);
}

function snapshotFrom(source: Cloud, withFree = true): EdgeEndpointSnapshot {
  return {
    positions: Float32Array.from(source.positions),
    normals: Float32Array.from(source.normals),
    confidence: Float32Array.from(source.confidence),
    residualMeters: Float32Array.from(source.residual),
    normalConfidence: Float32Array.from(source.normalConfidence),
    coverageConfidence: Float32Array.from(source.coverageConfidence),
    coverageReady: Uint8Array.from(source.ready),
    pointCount: source.confidence.length,
    voxelSizeMeters: 0.04,
    freeSpacePositions: withFree ? freeBeyondUpperEndpoint() : new Float32Array(0),
    freeSpaceVoxelSizeMeters: 0.05,
  };
}

function snapshot(withFree = true): EdgeEndpointSnapshot {
  return snapshotFrom(bedEdgeCloud(), withFree);
}

describe('multi-view structural edge endpoint evidence', () => {
  it('detects a finite two-plane endpoint only where verified FREE continues beyond it', () => {
    const observations = detectEdgeEndpointObservations(snapshot(true));
    const freeEndpoint = observations.find((candidate) => candidate.freeBeyond);
    expect(freeEndpoint).toBeDefined();
    expect(freeEndpoint!.position[0]).toBeCloseTo(0, 2);
    expect(freeEndpoint!.position[1]).toBeCloseTo(0, 2);
    expect(freeEndpoint!.position[2]).toBeGreaterThan(0.43);
    expect(freeEndpoint!.position[2]).toBeLessThan(0.53);
    expect(freeEndpoint!.support).toBeGreaterThanOrEqual(8);
  });

  it('rejects a dense endpoint on an edge whose measured overlap is below operational line scale', () => {
    // Plenty of samples exist at both ends, but the actual two-plane overlap is
    // only ~8 cm. Endpoint-corner inference must not bypass the 9-cm line gate.
    const short = snapshotFrom(edgeCloud(0.42, 0.50, 0.01), true);
    expect(short.pointCount).toBeGreaterThan(24);
    expect(detectEdgeEndpointObservations(short)).toHaveLength(0);
  });

  it('does not mature a two-plane endpoint from a single viewpoint', () => {
    const tracker = new StructuralEdgeEndpointTracker();
    const result = tracker.update(snapshot(true), {
      position: [0.45, -0.35, 0.42],
      forward: [-0.7, 0.6, 0],
      keyframeId: 10,
    });
    expect(result).toHaveLength(0);
  });

  it('does not mature from rotation/jitter while the camera origin barely moves', () => {
    const tracker = new StructuralEdgeEndpointTracker();
    const scene = snapshot(true);
    expect(tracker.update(scene, { position: [0.45, -0.35, 0.42], keyframeId: 10 })).toHaveLength(0);
    expect(tracker.update(scene, { position: [0.46, -0.35, 0.42], keyframeId: 14 })).toHaveLength(0);
    expect(tracker.update(scene, { position: [0.47, -0.35, 0.42], keyframeId: 18 })).toHaveLength(0);
  });

  it('matures a stable endpoint after three independent viewpoints with FREE beyond', () => {
    const tracker = new StructuralEdgeEndpointTracker();
    const scene = snapshot(true);
    expect(tracker.update(scene, { position: [0.45, -0.35, 0.42], keyframeId: 10 })).toHaveLength(0);
    expect(tracker.update(scene, { position: [-0.32, -0.42, 0.46], keyframeId: 14 })).toHaveLength(0);
    const result = tracker.update(scene, { position: [0.38, 0.34, 0.40], keyframeId: 18 });
    expect(result.length).toBeGreaterThan(0);
    expect(result[0].id).toContain('endpoint:');
    expect(result[0].observations).toBeGreaterThanOrEqual(3);
    expect(result[0].position[2]).toBeGreaterThan(0.43);
    expect(result[0].confidence).toBeGreaterThanOrEqual(0.70);
  });

  it('refuses to promote a scan frontier when no verified FREE exists beyond the endpoint', () => {
    const tracker = new StructuralEdgeEndpointTracker();
    const scene = snapshot(false);
    tracker.update(scene, { position: [0.45, -0.35, 0.42], keyframeId: 10 });
    tracker.update(scene, { position: [-0.32, -0.42, 0.46], keyframeId: 14 });
    const result = tracker.update(scene, { position: [0.38, 0.34, 0.40], keyframeId: 18 });
    expect(result).toHaveLength(0);
  });
});
