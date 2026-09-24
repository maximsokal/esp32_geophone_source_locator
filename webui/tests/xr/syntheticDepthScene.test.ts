import { describe, expect, it } from 'vitest';

import { DepthPointCloudAccumulator } from '../../src/xr/depthPointCloudV2';
import {
  ARCORE_NOISE, DEFAULT_INTRINSICS, NO_NOISE, boxCornerScene, creaseBinStats, dot, insideCornerScene,
  nearestPolygon, norm, normalize, orbitPoses, projectToImage, projectionMatrix, renderDepth, ridgeScene,
  scale, scanScene, slabScene, stepScene, sub, tiltedPlaneScene, vNotchScene, viewRay, add,
  type CameraPose, type SyntheticScene,
} from './support/syntheticDepthScene';

/* MAX-251: the harness itself must be physically consistent before any
 * accumulator/corner behaviour is judged with it. */

const VOXEL = 0.04;

function sameBits(a: Float32Array, b: Float32Array): boolean {
  if (a.length !== b.length) return false;
  const x = new Uint32Array(a.buffer, a.byteOffset, a.length);
  const y = new Uint32Array(b.buffer, b.byteOffset, b.length);
  for (let i = 0; i < x.length; i += 1) if (x[i] !== y[i]) return false;
  return true;
}

function allScenes(): SyntheticScene[] {
  return [
    boxCornerScene(), insideCornerScene(), ridgeScene(90), ridgeScene(30), vNotchScene(90),
    slabScene(), stepScene(), tiltedPlaneScene(35, 20),
  ];
}

describe('synthetic depth harness self-tests', () => {
  it('unprojects to points that lie on the analytic surface (fronto-parallel plane, rotated camera)', () => {
    // A plane facing the camera has constant view depth, so the runtime's
    // continuous-uv unprojection of the nearest-pixel depth must be exact.
    const scene = tiltedPlaneScene(30, 45, 2.0);
    const normal = scene.polygons[0].normal;
    const pose: CameraPose = { eye: add(scene.focus, scale(normal, 1.5)), target: scene.focus };
    const accumulator = new DepthPointCloudAccumulator();
    scanScene(accumulator, scene, [pose]);
    const snapshot = accumulator.snapshot(1, 1);
    expect(snapshot.tentativePointCount).toBeGreaterThan(300);
    for (let i = 0; i < snapshot.tentativePointCount; i += 1) {
      const p = [snapshot.tentativePositions[i * 3], snapshot.tentativePositions[i * 3 + 1], snapshot.tentativePositions[i * 3 + 2]] as const;
      // Float32 storage at ~1.5 m: 1e-4 m is far above rounding and far below a pixel (~1 cm).
      expect(Math.abs(dot(sub(p, scene.focus), normal))).toBeLessThan(1e-4);
    }
  });

  it('points the central ray at the look-at target for arbitrary rotations', () => {
    for (const eye of [[1, 0.2, 0.5], [-0.3, 1.1, 0.9], [0.4, -0.7, -0.2]] as const) {
      const pose: CameraPose = { eye, target: [0.1, -0.05, 0.02] };
      const ray = normalize(viewRay(pose, DEFAULT_INTRINSICS, 0.5, 0.5));
      const expected = normalize(sub(pose.target, pose.eye));
      expect(dot(ray, expected)).toBeGreaterThan(1 - 1e-9);
      const image = projectToImage(pose, DEFAULT_INTRINSICS, pose.target)!;
      // worldFromView is a Float32Array like the runtime's, hence ~1e-7 precision.
      expect(image[0]).toBeCloseTo(0.5, 6);
      expect(image[1]).toBeCloseTo(0.5, 6);
    }
  });

  it('renders real parallax: a sideways camera move shifts the step edge by the analytic amount', () => {
    const scene = stepScene(0.2, 2.0);
    const height = 1.5;
    const shift = 0.3;
    const a: CameraPose = { eye: [0, 0, height], target: [0, 0, 0] };
    const b: CameraPose = { eye: [shift, 0, height], target: [shift, 0, 0] };
    const edgeColumn = (buffer: Float32Array): number => {
      const row = DEFAULT_INTRINSICS.height >> 1;
      for (let column = 1; column < DEFAULT_INTRINSICS.width; column += 1) {
        const left = buffer[row * DEFAULT_INTRINSICS.width + column - 1];
        const right = buffer[row * DEFAULT_INTRINSICS.width + column];
        if (Math.abs(left - right) > 0.1) return column;
      }
      return -1;
    };
    const frameA = renderDepth(scene, a, 1);
    const frameB = renderDepth(scene, b, 2);
    const columnA = edgeColumn(frameA.buffer);
    const columnB = edgeColumn(frameB.buffer);
    // The upper-floor edge (x = 0, z = 0.2) seen from above: u = (1 + p00 * x_view / depth) / 2.
    const p00 = projectionMatrix(DEFAULT_INTRINSICS)[0];
    const depth = height - 0.2;
    const expectedA = ((1 + (p00 * 0) / depth) / 2) * DEFAULT_INTRINSICS.width;
    const expectedB = ((1 + (p00 * -shift) / depth) / 2) * DEFAULT_INTRINSICS.width;
    expect(Math.abs(columnA - expectedA)).toBeLessThanOrEqual(1);
    expect(Math.abs(columnB - expectedB)).toBeLessThanOrEqual(1);
    expect(columnA - columnB).toBeGreaterThan(10);
  });

  it('has unit outward normals, each visible from the default orbit rig', () => {
    for (const scene of allScenes()) {
      for (const polygon of scene.polygons) {
        expect(norm(polygon.normal)).toBeCloseTo(1, 12);
        const centroid = scale(polygon.vertices.reduce((sum, v) => add(sum, v), [0, 0, 0] as const), 1 / polygon.vertices.length);
        // Every vertex lies in the polygon plane.
        for (const vertex of polygon.vertices) expect(Math.abs(dot(sub(vertex, centroid), polygon.normal))).toBeLessThan(1e-12);
        // Downward-facing faces (slab bottom) need the low orbit.
        const poses = [...orbitPoses(scene.focus, 1.5, 50, 0, 315, 8), ...orbitPoses(scene.focus, 1.5, -50, 0, 315, 8)];
        expect(poses.some((pose) => dot(sub(pose.eye, centroid), polygon.normal) > 0)).toBe(true);
      }
    }
  });

  it('is deterministic: the same noisy scan gives bit-identical snapshots', () => {
    const run = (): Float32Array => {
      const scene = ridgeScene(90);
      const accumulator = new DepthPointCloudAccumulator();
      scanScene(accumulator, scene, orbitPoses(scene.focus, 1.5, 55, 0, 315, 8), { noise: ARCORE_NOISE });
      return accumulator.snapshot(8, 8).positions;
    };
    const first = run();
    const second = run();
    expect(first.length).toBeGreaterThan(100);
    expect(sameBits(first, second)).toBe(true);
  });

  it('applies the seeded noise model (enabled vs disabled differ, sigma matches d^2 law)', () => {
    const scene = tiltedPlaneScene(0, 0, 4);
    const pose: CameraPose = { eye: [0, 0, 2], target: [0, 0, 0] };
    const exact = renderDepth(scene, pose, 1, NO_NOISE).buffer;
    const noisy = renderDepth(scene, pose, 1, ARCORE_NOISE).buffer;
    let sum = 0; let sumSq = 0; let n = 0;
    for (let i = 0; i < exact.length; i += 1) {
      if (!Number.isFinite(exact[i]) || !Number.isFinite(noisy[i])) continue;
      const e = noisy[i] - exact[i]; sum += e; sumSq += e * e; n += 1;
    }
    const sigma = Math.sqrt(sumSq / n - (sum / n) ** 2);
    // Depth is ~2 m everywhere -> sigma = 0.0015 * 4 = 6 mm (+ 1 mm quantisation, ~0.3 mm rms).
    expect(n).toBeGreaterThan(10000);
    expect(sigma).toBeGreaterThan(0.0045);
    expect(sigma).toBeLessThan(0.0085);
    const otherSeed = renderDepth(scene, pose, 1, { ...ARCORE_NOISE, seed: 7 }).buffer;
    expect(sameBits(otherSeed, noisy)).toBe(false);
  });

  it('drops depth at grazing incidence beyond 80 degrees', () => {
    const scene = tiltedPlaneScene(0, 0, 40);
    // Camera 0.2 m above a huge floor looking far away: far pixels are grazing.
    const pose: CameraPose = { eye: [0, 0, 0.2], target: [10, 0, 0.1] };
    const buffer = renderDepth(scene, pose, 1, ARCORE_NOISE).buffer;
    const exact = renderDepth(scene, pose, 1, NO_NOISE).buffer;
    let dropped = 0;
    for (let i = 0; i < buffer.length; i += 1) if (Number.isFinite(exact[i]) && !Number.isFinite(buffer[i])) dropped += 1;
    expect(dropped).toBeGreaterThan(100);
  });
});

describe('synthetic depth harness baselines', () => {
  it('baseline: a 90 deg ridge scanned from 8 orbit poses is not rounded (< 5 mm near the crease)', () => {
    // Locks in the MAX-250 non-bug: samples from both faces do not merge into a chamfer.
    const scene = ridgeScene(90);
    const accumulator = new DepthPointCloudAccumulator(VOXEL);
    scanScene(accumulator, scene, orbitPoses(scene.focus, 1.5, 55, 0, 315, 8), { noise: ARCORE_NOISE });
    const snapshot = accumulator.snapshot(8, 8);
    const bins = creaseBinStats(scene, { positions: snapshot.positions, normals: snapshot.normals, count: snapshot.pointCount }, VOXEL);
    const nearCrease = bins[0];
    // The 1-voxel band is 2 x 0.6 m x 0.04 m = 10 % of the 0.48 m^2 ridge; demand at
    // least half of that share so the mean below is not computed from a handful of points.
    expect(nearCrease.count).toBeGreaterThanOrEqual(Math.floor(snapshot.pointCount * 0.10 * 0.5));
    expect(nearCrease.count).toBeGreaterThanOrEqual(5);
    expect(nearCrease.meanSurfaceDistanceM).toBeLessThan(0.005);
    // Faces away from the crease stay within the depth noise (sigma ~4 mm at 1.5-1.7 m).
    expect(bins[3].p95SurfaceDistanceM).toBeLessThan(0.012);
  });

  it('nearestPolygon/creaseBinStats report exact values on analytic points', () => {
    const scene = boxCornerScene([0, 0, 0], 0.4);
    const onFace = [0, -0.2, -0.2] as const;
    expect(nearestPolygon(scene, onFace).distance).toBeLessThan(1e-12);
    expect(nearestPolygon(scene, [0.01, -0.2, -0.2]).distance).toBeCloseTo(0.01, 12);
    const stats = creaseBinStats(scene, { positions: [0, -0.2, -0.2, 0, -0.01, -0.2], normals: [1, 0, 0, 1, 0, 0], count: 2 }, VOXEL);
    expect(stats[0].count).toBe(1); // 1 cm from the x|y crease = 0.25 voxel
    expect(stats[3].count).toBe(1); // 20 cm from every crease = 5 voxels
    expect(stats[0].meanNormalErrorDeg).toBeCloseTo(0, 9);
  });
});
