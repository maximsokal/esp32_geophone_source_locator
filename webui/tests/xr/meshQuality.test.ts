import { describe, expect, it } from 'vitest';

import { buildDirectionalTsdfMesh } from '../../src/xr/directionalTsdf';
import { sampleMesh, shareNear, surfaceQuality, topologyMetrics } from '../support/meshMetrics';
import {
  cornerScene, cubeScene, doorwayScene, mixedDensityScene, noisyScene, parallelWallsScene, planeScene, sphereScene, thinBoardScene, twoObjectsScene,
  type SyntheticSurfaceScene,
} from '../support/syntheticSurfaces';

/*
 * Quality of the post-scan mesh on public, made-up scenes with a KNOWN true surface (no real place is involved). Measured on the mesh AS DISPLAYED: after the
 * scalar surface, the directional pass for what it left uncovered, and the final refinement. The tolerance is the resolution of the reconstruction: twice
 * its TSDF voxel (the scan voxel 0.048 m becomes 0.055 m), 0.11 m.
 */
const VOXEL = 0.048;
const TOLERANCE = 2 * VOXEL * 1.15;

function reconstruct(scene: SyntheticSurfaceScene, maxTriangles = 40000) {
  return buildDirectionalTsdfMesh({
    positions: scene.points, normals: scene.normals, confidence: new Float32Array(scene.points.length / 3).fill(0.9), voxelSizeMeters: VOXEL, maxTriangles, maxInputPoints: 12000,
  });
}
function samplesInVoids(soup: Float32Array, scene: SyntheticSurfaceScene): number {
  const samples = sampleMesh(soup, 'dense');
  let count = 0;
  for (const v of scene.voids) for (let i = 0; i < samples.length; i += 3) if (Math.hypot(samples[i] - v.centre[0], samples[i + 1] - v.centre[1], samples[i + 2] - v.centre[2]) < v.radius) count += 1;
  return count;
}

const FULLY_MEASURED: Array<[string, () => SyntheticSurfaceScene, number, number]> = [
  ['MESH-01 one plane', planeScene, 1, 0],
  ['MESH-03/04 floor and two walls meeting in an inside corner', cornerScene, 1, 0],
  ['MESH-06 a cube', cubeScene, 1, 0],
  ['MESH-07 a sphere (the old directional pass returned NO triangle)', sphereScene, 1, 0],
  ['MESH-08 two objects 15 cm apart stay two objects', twoObjectsScene, 2, 0],
  ['MESH-09 a doorway stays open', doorwayScene, 1, 0],
  ['MESH-11 two parallel boards 15 cm apart do not merge', parallelWallsScene, 2, 0],
  ['MESH-14 noisy points (4 mm, 2% outliers): no bridges', () => noisyScene(), 1, 3],
  ['a thin board measured from both sides keeps both layers', thinBoardScene, 2, 0],
];

describe('post-scan mesh quality on scenes with a known surface', () => {
  it.each(FULLY_MEASURED)('%s: precision and recall at least 95%%, no void filled, no degenerate triangle', (_label, make, components, nonManifold) => {
    const scene = make();
    const mesh = reconstruct(scene);
    const quality = surfaceQuality(mesh.trianglePositions, scene.truth, TOLERANCE);
    const topology = topologyMetrics(mesh.trianglePositions);
    expect(mesh.triangleCount, 'a mesh exists').toBeGreaterThan(200);
    expect(quality.precision, 'what is drawn is real').toBeGreaterThanOrEqual(0.95);
    expect(quality.recall, 'what exists is drawn').toBeGreaterThanOrEqual(0.95);
    expect(topology.components, 'no false bridge, no needless split').toBe(components);
    expect(samplesInVoids(mesh.trianglePositions, scene), 'nothing drawn where there is no surface').toBe(0);
    expect(topology.degenerateTriangles).toBe(0);
    expect(mesh.selfIntersectionPairCount).toBe(0);
    expect(topology.nonManifoldEdges).toBeLessThanOrEqual(nonManifold);
  });

  it('MESH-16 a region sampled only every 9 cm is not painted over: the dense half is reconstructed, what is drawn stays real', () => {
    const scene = mixedDensityScene();
    const mesh = reconstruct(scene);
    const denseHalf = new Float32Array(scene.truth.length);
    let kept = 0;
    for (let i = 0; i < scene.truth.length; i += 3) if (scene.truth[i] < 1.0) { denseHalf.set(scene.truth.subarray(i, i + 3), kept); kept += 3; }
    const dense = surfaceQuality(mesh.trianglePositions, denseHalf.slice(0, kept), TOLERANCE);
    expect(dense.recall).toBeGreaterThanOrEqual(0.95);
    expect(surfaceQuality(mesh.trianglePositions, scene.truth, TOLERANCE).precision).toBeGreaterThanOrEqual(0.95);
    expect(mesh.directionalFallbackPoints, 'the sparse points are the "not explored enough" evidence').toBeGreaterThan(100);
  });

  it('MESH-17 / unsupported surface: every drawn sample lies within the tolerance of a measured point (nothing is invented between the pieces)', () => {
    for (const make of [cornerScene, twoObjectsScene, doorwayScene]) {
      const scene = make();
      const mesh = reconstruct(scene);
      expect(shareNear(sampleMesh(mesh.trianglePositions, 'light'), scene.points, TOLERANCE)).toBeGreaterThanOrEqual(0.95);
    }
  });

  it('MESH-20 the same input gives the same mesh', () => {
    const scene = cornerScene();
    const a = reconstruct(scene); const b = reconstruct(scene);
    expect(Array.from(a.trianglePositions)).toEqual(Array.from(b.trianglePositions));
  });

  it('MESH-21 the metrics describe the displayed mesh: they agree with an independent measurement of the very triangles returned', () => {
    for (const make of [planeScene, cubeScene, twoObjectsScene]) {
      const mesh = reconstruct(make());
      const independent = topologyMetrics(mesh.trianglePositions);
      expect(mesh.finalTopology!.triangles).toBe(mesh.triangleCount);
      expect(mesh.finalTopology!.components).toBe(independent.components);
      expect(mesh.meshTopologyComponentCount).toBe(independent.components);
      expect(mesh.finalTopology!.degenerateTriangles).toBe(0);
      expect(Math.abs(mesh.finalTopology!.boundaryEdgeLengthMeters - independent.boundaryEdgeLengthMeters) / Math.max(1, independent.boundaryEdgeLengthMeters)).toBeLessThan(0.05);
    }
  });

  it('the triangle budget is ONE budget: the directional additions fit in what the scalar surface left, never beyond it', () => {
    const scene = thinBoardScene();                                                              /* the scalar field builds nothing here: the whole budget is the directional pass's */
    for (const budget of [600, 1500, 5000]) {
      const mesh = reconstruct(scene, budget);
      expect(mesh.triangleCount, `budget ${budget}`).toBeLessThanOrEqual(budget);
      expect(mesh.triangleCount).toBeGreaterThan(0);
    }
    const mixed = reconstruct(cornerScene(), 4000);
    expect(mixed.triangleCount).toBeLessThanOrEqual(4000);
  });

  it('MESH-22 exhausting the triangle budget never drops a whole part of the scene: the surface gets coarser everywhere instead of ending in bands', () => {
    const scene = planeScene();
    const roomy = reconstruct(scene);
    const tight = reconstruct(scene, 900);
    expect(tight.triangleCount).toBeLessThanOrEqual(900);
    const quality = surfaceQuality(tight.trianglePositions, scene.truth, TOLERANCE);
    expect(quality.recall, 'the whole plane is still covered').toBeGreaterThanOrEqual(0.9);
    expect(topologyMetrics(tight.trianglePositions).components).toBe(topologyMetrics(roomy.trianglePositions).components);
  });
});
