import { describe, expect, it } from 'vitest';

import { DepthPointCloudAccumulator } from '../../src/xr/depthPointCloudV2';
import {
  ARCORE_NOISE, boxCornerScene, creaseBinStats, distanceToCrease, insideCornerScene, nearestPolygon,
  normalErrorDeg, orbitPoses, percentile, ridgeScene, scanScene, sceneNormalErrorDeg, slabScene, stepScene,
  rectangle, tiltedPlaneScene, vNotchScene, wrongNormalPassingGate,
  type CameraPose, type SyntheticScene, type Vec3,
} from './support/syntheticDepthScene';

/*
 * MAX-253: no blended high-confidence normals at creases, no fabricated normal
 * on grazing faces. Ground truth comes from the analytic scene; a point within
 * 1 cm of two faces may carry either face's normal (sceneNormalErrorDeg).
 */

const VOXEL = 0.04;

/**
 * Louvre of nine floating slats ~2 depth pixels wide, each tilted 45 deg about
 * its long axis: across a slat there is often no usable neighbour, so some
 * orientations are genuinely unmeasurable. True normals are 45 deg from +Z.
 */
function louvre(): SyntheticScene {
  const half = 0.02;
  return {
    name: 'louvre',
    polygons: Array.from({ length: 9 }, (_, i) => rectangle([0, (i - 4) * 0.1, 0.3], [0.25, 0, 0], [0, half * Math.SQRT1_2, half * Math.SQRT1_2], `slat${i}`)),
    creases: [], corners: [], focus: [0, 0, 0.3],
  };
}
const LOUVRE_POSES = orbitPoses([0, 0, 0.3], 1.2, 70, 60, 120, 10);

const STRUCTURAL_GATE = 0.68; // structuralGuide.ts MIN_POINT_CONFIDENCE

function guide(scene: SyntheticScene, poses: readonly CameraPose[]) {
  const accumulator = new DepthPointCloudAccumulator(VOXEL);
  scanScene(accumulator, scene, poses, { noise: ARCORE_NOISE });
  return { accumulator, structural: accumulator.structuralGuideSnapshot(), final: accumulator.snapshot(poses.length, poses.length) };
}

/** Top-down sideways walk 1.5 m above the plane centre: incidence == plane tilt. */
function topDownWalk(): CameraPose[] {
  return [[0, 0], [0.45, 0], [-0.45, 0], [0.25, 0.2], [-0.25, -0.2], [0.1, 0.35], [-0.3, 0.3], [0.35, -0.3]]
    .map(([x, y]) => ({ eye: [x, y, 1.5] as Vec3, target: [x, y + 0.001, 0] as Vec3 }));
}

describe('surface normals at creases (structural export)', () => {
  const cases: Array<[SyntheticScene, CameraPose[]]> = [
    [ridgeScene(90), orbitPoses(ridgeScene(90).focus, 1.5, 55, 0, 315, 8)],
    [boxCornerScene(), orbitPoses(boxCornerScene().focus, 1.4, 35, 10, 80, 10)],
  ];
  for (const [scene, poses] of cases) {
    it(`${scene.name}: crease band p90 <= 15 deg, faces p90 <= 6 deg`, () => {
      const { structural } = guide(scene, poses);
      const set = { positions: structural.positions, normals: structural.normals, count: structural.pointCount };
      const creaseBand = creaseBinStats(scene, set, VOXEL)[0];
      // At least 5 points so the percentile is about a band, not a single sample.
      expect(creaseBand.count).toBeGreaterThanOrEqual(5);
      expect(creaseBand.p90NormalErrorDeg).toBeLessThanOrEqual(15);
      const faceErrors: number[] = [];
      for (let i = 0; i < structural.pointCount; i += 1) {
        const p: Vec3 = [structural.positions[i * 3], structural.positions[i * 3 + 1], structural.positions[i * 3 + 2]];
        if (distanceToCrease(scene, p) <= 0.05) continue;
        faceErrors.push(sceneNormalErrorDeg(scene, p, [structural.normals[i * 3], structural.normals[i * 3 + 1], structural.normals[i * 3 + 2]]));
      }
      expect(faceErrors.length).toBeGreaterThan(20);
      expect(percentile(faceErrors, 0.9)).toBeLessThanOrEqual(6);
    });
  }
});

describe('surface normals on grazing faces', () => {
  for (const tilt of [0, 45, 60, 70, 78]) {
    it(`tilt ${tilt} deg: no wrong normal passes the structural gate, and the face is not dropped`, () => {
      const scene = tiltedPlaneScene(tilt, 0, 1.2);
      const { accumulator, structural } = guide(scene, topDownWalk());
      expect(wrongNormalPassingGate(scene, { positions: structural.positions, normals: structural.normals, confidence: structural.confidence, count: structural.pointCount }, STRUCTURAL_GATE, 20)).toBe(0);
      // Marking every hard point "unknown" would also satisfy the line above;
      // the structural export must keep >= 90 % of the promoted surface.
      expect(accumulator.promotedCount).toBeGreaterThan(100);
      expect(structural.pointCount).toBeGreaterThanOrEqual(Math.ceil(accumulator.promotedCount * 0.9));
    });
  }
});

describe('no fabricated normals anywhere', () => {
  it('never exports exactly +Z unless the true surface normal is within 5 deg of +Z', () => {
    const scenes: Array<[SyntheticScene, CameraPose[]]> = [
      [ridgeScene(90), orbitPoses(ridgeScene(90).focus, 1.5, 55, 0, 315, 8)],
      [vNotchScene(90), orbitPoses(vNotchScene(90).focus, 1.4, 70, 0, 315, 8)],
      [boxCornerScene(), orbitPoses(boxCornerScene().focus, 1.4, 35, 10, 80, 10)],
      [insideCornerScene(), orbitPoses(insideCornerScene().focus, 1.4, 35, 10, 80, 8)],
      [slabScene(), orbitPoses(slabScene().focus, 1.2, 30, -60, 60, 8)],
      [stepScene(), orbitPoses(stepScene().focus, 1.4, 60, 150, 210, 6)],
      ...[60, 70, 78].map((tilt): [SyntheticScene, CameraPose[]] => [tiltedPlaneScene(tilt, 0, 1.2), topDownWalk()]),
      [louvre(), LOUVRE_POSES],
    ];
    let checked = 0;
    for (const [scene, poses] of scenes) {
      const { structural, final } = guide(scene, poses);
      for (const set of [
        { positions: structural.positions, normals: structural.normals, count: structural.pointCount },
        { positions: final.positions, normals: final.normals, count: final.pointCount },
      ]) {
        for (let i = 0; i < set.count; i += 1) {
          checked += 1;
          const n: Vec3 = [set.normals[i * 3], set.normals[i * 3 + 1], set.normals[i * 3 + 2]];
          if (n[0] !== 0 || n[1] !== 0 || n[2] !== 1) continue;
          const p: Vec3 = [set.positions[i * 3], set.positions[i * 3 + 1], set.positions[i * 3 + 2]];
          expect(normalErrorDeg([0, 0, 1], nearestPolygon(scene, p).polygon.normal), `${scene.name} point ${i}`).toBeLessThanOrEqual(5);
        }
      }
    }
    expect(checked).toBeGreaterThan(1000);
  });

  it('marks unmeasurable orientations as unknown (confidence 0), keeps them out of the structural export', () => {
    const scene = louvre();
    const accumulator = new DepthPointCloudAccumulator(VOXEL);
    scanScene(accumulator, scene, LOUVRE_POSES, { noise: ARCORE_NOISE });
    const final = accumulator.snapshot(LOUVRE_POSES.length, LOUVRE_POSES.length);
    const structural = accumulator.structuralGuideSnapshot();
    expect(final.normalConfidence).toBeDefined();
    let unknown = 0;
    for (let i = 0; i < final.pointCount; i += 1) {
      const confidence = final.normalConfidence![i];
      expect(confidence).toBeGreaterThanOrEqual(0);
      expect(confidence).toBeLessThanOrEqual(1);
      const p: Vec3 = [final.positions[i * 3], final.positions[i * 3 + 1], final.positions[i * 3 + 2]];
      const error = sceneNormalErrorDeg(scene, p, [final.normals[i * 3], final.normals[i * 3 + 1], final.normals[i * 3 + 2]]);
      if (confidence === 0) unknown += 1;
      // Any normal claimed with confidence must be a real measurement.
      else expect(error).toBeLessThan(25);
    }
    // The scene is built so that the unknown path is really exercised...
    expect(unknown).toBeGreaterThan(0);
    // ...and those points never reach plane/edge fitting.
    expect(structural.pointCount).toBeLessThanOrEqual(final.pointCount - unknown);
  });
});
