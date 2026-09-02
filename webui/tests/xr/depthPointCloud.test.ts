import { describe, expect, it } from 'vitest';
import {
  DepthPointCloudAccumulator,
  type CpuDepthInformationLike,
  type DepthFusionFrameContext,
  type DepthSampleView,
} from '../../src/xr/depthPointCloudV2';

const projection = new Float32Array([
  1, 0, 0, 0,
  0, 1, 0, 0,
  0, 0, 1, 0,
  0, 0, 0, 1,
]);
const identity = new Float32Array(projection);
const view: DepthSampleView = { projectionMatrix: projection, transform: { matrix: identity } };

function depthPlane(sample: (u: number, v: number) => number): CpuDepthInformationLike {
  return { width: 160, height: 90, getDepthInMeters: sample };
}

function context(
  keyframeId: number,
  evidenceKeyframe: boolean,
  cameraProjectPosition: readonly [number, number, number] = [0, 0, 0],
  frameWeight = 1,
): DepthFusionFrameContext {
  return { keyframeId, evidenceKeyframe, frameWeight, cameraProjectPosition };
}

function meanDepth(positions: Float32Array): number {
  let total = 0;
  for (let index = 1; index < positions.length; index += 3) total += positions[index];
  return total / Math.max(1, positions.length / 3);
}

function assertFiniteArray(values: Float32Array): void {
  for (const value of values) expect(Number.isFinite(value)).toBe(true);
}

describe('DepthPointCloudAccumulator basic invariants', () => {
  it('starts empty and resets all retained geometry', () => {
    const accumulator = new DepthPointCloudAccumulator();
    expect(accumulator.pointCount).toBe(0);
    accumulator.sampleDepthFrame(depthPlane(() => 1.5), view, context(1, true));
    expect(accumulator.pointCount).toBeGreaterThan(100);
    accumulator.reset();
    expect(accumulator.pointCount).toBe(0);
    expect(accumulator.promotedCount).toBe(0);
    expect(accumulator.keyframeCount).toBe(0);
    expect(accumulator.polishFrameCount).toBe(0);
  });

  it('rejects a malformed projection matrix without mutating the map', () => {
    const accumulator = new DepthPointCloudAccumulator();
    const badView: DepthSampleView = { projectionMatrix: new Float32Array(4), transform: { matrix: identity } };
    expect(accumulator.sampleDepthFrame(depthPlane(() => 1.5), badView, context(1, true)).changedPoints).toBe(0);
    expect(accumulator.pointCount).toBe(0);
  });

  it('rejects invalid focal matrix entries', () => {
    const accumulator = new DepthPointCloudAccumulator();
    const badProjection = new Float32Array(projection);
    badProjection[0] = 0;
    const badView: DepthSampleView = { projectionMatrix: badProjection, transform: { matrix: identity } };
    expect(accumulator.sampleDepthFrame(depthPlane(() => 1.5), badView, context(1, true)).acceptedSamples).toBe(0);
  });

  it('ignores depths below the near validity threshold', () => {
    const accumulator = new DepthPointCloudAccumulator();
    accumulator.sampleDepthFrame(depthPlane(() => 0.05), view, context(1, true));
    expect(accumulator.pointCount).toBe(0);
  });

  it('respects the configured maximum depth', () => {
    const accumulator = new DepthPointCloudAccumulator(0.04, 60000, 2);
    accumulator.sampleDepthFrame(depthPlane(() => 3), view, context(1, true));
    expect(accumulator.pointCount).toBe(0);
  });

  it('clamps runtime max depth to the supported 1..8m interval', () => {
    const accumulator = new DepthPointCloudAccumulator();
    accumulator.setMaxDepthMeters(100);
    expect(accumulator.maxDepth).toBe(8);
    accumulator.setMaxDepthMeters(0.1);
    expect(accumulator.maxDepth).toBe(1);
  });

  it('never emits NaN coordinates or confidence from a valid plane', () => {
    const accumulator = new DepthPointCloudAccumulator();
    const flat = depthPlane(() => 1.5);
    accumulator.sampleDepthFrame(flat, view, context(1, true));
    accumulator.sampleDepthFrame(flat, view, context(2, true, [0.45, 0, 0]));
    const snapshot = accumulator.snapshot(2, 2);
    assertFiniteArray(snapshot.positions);
    assertFiniteArray(snapshot.normals);
    assertFiniteArray(snapshot.confidence);
    assertFiniteArray(snapshot.tentativePositions);
    assertFiniteArray(snapshot.tentativeConfidence);
  });
});

describe('independent evidence and anti-saturation', () => {
  it('does not promote geometry just because global keyframe ids keep increasing', () => {
    const accumulator = new DepthPointCloudAccumulator();
    const flat = depthPlane(() => 1.5);
    accumulator.sampleDepthFrame(flat, view, context(1, true));
    const created = accumulator.pointCount;
    for (let keyframeId = 2; keyframeId <= 12; keyframeId += 1) {
      accumulator.sampleDepthFrame(flat, view, context(keyframeId, true));
    }
    expect(accumulator.promotedCount).toBe(0);
    expect(accumulator.pointCount).toBeLessThanOrEqual(created * 3);
  });

  it('keeps stationary unpromoted geometry visually tentative instead of green', () => {
    const accumulator = new DepthPointCloudAccumulator();
    const flat = depthPlane(() => 1.5);
    accumulator.sampleDepthFrame(flat, view, context(1, true));
    accumulator.sampleDepthFrame(flat, view, context(2, true));
    const guide = accumulator.liveCoverageSnapshot();
    expect(guide.active).toBe(true);
    expect(guide.promotedPointCount).toBe(0);
    expect(guide.pointCount).toBeGreaterThan(0);
    expect(Math.max(...guide.confidence)).toBeLessThanOrEqual(0.46);
  });

  it('promotes surfaces after a real side baseline creates independent view evidence', () => {
    const accumulator = new DepthPointCloudAccumulator();
    const flat = depthPlane(() => 1.5);
    accumulator.sampleDepthFrame(flat, view, context(1, true, [0, 0, 0]));
    accumulator.sampleDepthFrame(flat, view, context(2, true, [0.45, 0, 0]));
    expect(accumulator.promotedCount).toBeGreaterThan(0);
    const snapshot = accumulator.snapshot(2, 2);
    expect(snapshot.pointCount).toBeGreaterThan(0);
    expect(Math.max(...snapshot.confidence)).toBeGreaterThan(0.5);
  });

  it('allows polish frames to refine matched surfaces but never create new geometry', () => {
    const accumulator = new DepthPointCloudAccumulator();
    const flat = depthPlane(() => 1.5);
    accumulator.sampleDepthFrame(flat, view, context(1, true, [0, 0, 0]));
    accumulator.sampleDepthFrame(flat, view, context(2, true, [0.45, 0, 0]));
    const before = accumulator.pointCount;
    for (let index = 0; index < 12; index += 1) {
      accumulator.sampleDepthFrame(flat, view, context(2, false, [0.45, 0, 0], 0.15));
    }
    expect(accumulator.pointCount).toBe(before);
    expect(accumulator.polishFrameCount).toBe(12);
  });

  it('does not let many polish frames manufacture independent view count', () => {
    const accumulator = new DepthPointCloudAccumulator();
    const flat = depthPlane(() => 1.5);
    accumulator.sampleDepthFrame(flat, view, context(1, true));
    const before = accumulator.promotedCount;
    for (let index = 0; index < 40; index += 1) {
      accumulator.sampleDepthFrame(flat, view, context(1, false, [0, 0, 0], 0.18));
    }
    expect(accumulator.promotedCount).toBe(before);
  });

  it('keeps all first-view hypotheses in the tentative output layer', () => {
    const accumulator = new DepthPointCloudAccumulator();
    accumulator.sampleDepthFrame(depthPlane(() => 1.5), view, context(1, true));
    const snapshot = accumulator.snapshot(1, 1);
    expect(snapshot.pointCount).toBe(0);
    expect(snapshot.tentativePointCount).toBe(accumulator.pointCount);
  });
});

describe('surface reconstruction behaviour', () => {
  it('preserves both sides of a sharp depth step', () => {
    const accumulator = new DepthPointCloudAccumulator();
    const edge = depthPlane((u) => (u < 0.5 ? 1.0 : 2.0));
    accumulator.sampleDepthFrame(edge, view, context(1, true, [0, 0, 0]));
    accumulator.sampleDepthFrame(edge, view, context(2, true, [0.45, 0, 0]));
    accumulator.sampleDepthFrame(edge, view, context(3, true, [-0.45, 0, 0]));
    const snapshot = accumulator.snapshot(3, 3);
    expect(snapshot.pointCount).toBeGreaterThan(50);
    expect(snapshot.bounds).not.toBeNull();
    expect((snapshot.bounds?.max[1] ?? 0) - (snapshot.bounds?.min[1] ?? 0)).toBeGreaterThan(0.65);
  });

  it('rejects isolated single-pixel-like depth spikes instead of promoting them', () => {
    const accumulator = new DepthPointCloudAccumulator();
    const spike = depthPlane((u, v) => (Math.abs(u - 0.5) < 0.01 && Math.abs(v - 0.5) < 0.01 ? 0.55 : 1.5));
    const cameras: Array<readonly [number, number, number]> = [[0, 0, 0], [0.45, 0, 0], [-0.45, 0, 0]];
    cameras.forEach((camera, index) => accumulator.sampleDepthFrame(spike, view, context(index + 1, true, camera)));
    const snapshot = accumulator.snapshot(3, 3);
    let suspicious = 0;
    for (let index = 1; index < snapshot.positions.length; index += 3) if (snapshot.positions[index] < 0.8) suspicious += 1;
    expect(suspicious).toBeLessThan(5);
  });

  it('self-polishes an already promoted noisy plane toward later consistent depth', () => {
    const accumulator = new DepthPointCloudAccumulator();
    const noisy = depthPlane(() => 1.53);
    const truth = depthPlane(() => 1.5);
    accumulator.sampleDepthFrame(noisy, view, context(1, true, [0, 0, 0]));
    accumulator.sampleDepthFrame(noisy, view, context(2, true, [0.45, 0, 0]));
    const before = accumulator.snapshot(2, 2);
    const beforeError = Math.abs(meanDepth(before.positions) - 1.5);
    for (let index = 0; index < 24; index += 1) {
      accumulator.sampleDepthFrame(truth, view, context(2, false, [0.45, 0, 0], 0.18));
    }
    const after = accumulator.snapshot(26, 26);
    const afterError = Math.abs(meanDepth(after.positions) - 1.5);
    expect(after.pointCount).toBe(before.pointCount);
    expect(afterError).toBeLessThan(beforeError * 0.85);
  });

  it('retains a narrow protrusion smaller than the old 1/40 image-grid column', () => {
    const accumulator = new DepthPointCloudAccumulator();
    const protrusion = depthPlane((u, v) =>
      u > 0.490 && u < 0.510 && v > 0.25 && v < 0.75 ? 1.0 : 1.5,
    );
    const cameras: Array<readonly [number, number, number]> = [
      [0, 0, 0], [0.45, 0, 0], [-0.45, 0, 0], [0.30, 0.20, 0], [-0.30, 0.20, 0],
    ];
    cameras.forEach((camera, index) => accumulator.sampleDepthFrame(protrusion, view, context(index + 1, true, camera)));
    const snapshot = accumulator.snapshot(cameras.length, cameras.length);
    let protrusionPoints = 0; let wallPoints = 0;
    for (let index = 1; index < snapshot.positions.length; index += 3) {
      const depth = snapshot.positions[index];
      if (depth < 1.2) protrusionPoints += 1;
      if (depth > 1.35) wallPoints += 1;
    }
    expect(protrusionPoints).toBeGreaterThan(3);
    expect(wallPoints).toBeGreaterThan(100);
  });

  it('produces meaningful normals on a planar surface', () => {
    const accumulator = new DepthPointCloudAccumulator();
    const flat = depthPlane(() => 1.5);
    accumulator.sampleDepthFrame(flat, view, context(1, true, [0, 0, 0]));
    accumulator.sampleDepthFrame(flat, view, context(2, true, [0.45, 0, 0]));
    const snapshot = accumulator.snapshot(2, 2);
    let unitLike = 0;
    for (let index = 0; index < snapshot.normals.length; index += 3) {
      const length = Math.hypot(snapshot.normals[index], snapshot.normals[index + 1], snapshot.normals[index + 2]);
      if (Math.abs(length - 1) < 0.05) unitLike += 1;
    }
    expect(unitLike).toBeGreaterThan(snapshot.pointCount * 0.8);
  });

  it('hemisphere-aligns opposite-view normals before multi-view fusion', () => {
    const accumulator = new DepthPointCloudAccumulator();
    const flat = depthPlane(() => 1.5);
    // Keep the same physical surface inside the residual gate, but add a small
    // tangent slope on the opposite-side observation. Without hemisphere
    // alignment, near-cancellation of n and -n magnifies that tiny slope into a
    // badly tilted fused normal.
    const slightlySloped = depthPlane((u) => 1.5 + 0.06 * (u - 0.5));
    accumulator.sampleDepthFrame(flat, view, context(1, true, [0, 0, 0]));
    accumulator.sampleDepthFrame(slightlySloped, view, context(2, true, [0, 3, 0]));
    const snapshot = accumulator.snapshot(2, 2);
    expect(snapshot.pointCount).toBeGreaterThan(50);
    let stableDominantAxis = 0;
    for (let index = 0; index < snapshot.normals.length; index += 3) {
      const nx = snapshot.normals[index];
      const ny = snapshot.normals[index + 1];
      const nz = snapshot.normals[index + 2];
      const length = Math.hypot(nx, ny, nz);
      if (Math.abs(length - 1) < 0.05 && Math.abs(ny) >= 0.98) stableDominantAxis += 1;
    }
    expect(stableDominantAxis).toBeGreaterThan(snapshot.pointCount * 0.75);
  });

  it('keeps a hard memory budget under a long translated scan', () => {
    const accumulator = new DepthPointCloudAccumulator(0.04, 1000, 5);
    const flat = depthPlane(() => 1.5);
    for (let keyframeId = 1; keyframeId <= 35; keyframeId += 1) {
      const translation = keyframeId * 0.22;
      const transform = new Float32Array([
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        translation, 0, 0, 1,
      ]);
      accumulator.sampleDepthFrame(
        flat,
        { projectionMatrix: projection, transform: { matrix: transform } },
        context(keyframeId, true, [translation, 0, 0]),
      );
      expect(accumulator.pointCount).toBeLessThanOrEqual(1000);
    }
  });

  it('stays bounded across a deterministic family of noisy planes', () => {
    for (let seed = 1; seed <= 8; seed += 1) {
      const accumulator = new DepthPointCloudAccumulator(0.04, 2500, 5);
      const noisy = depthPlane((u, v) => 1.5 + Math.sin((u * 37 + v * 19 + seed) * 3.1) * 0.006);
      accumulator.sampleDepthFrame(noisy, view, context(1, true, [0, 0, 0]));
      accumulator.sampleDepthFrame(noisy, view, context(2, true, [0.4, 0, 0]));
      accumulator.sampleDepthFrame(noisy, view, context(3, true, [-0.4, 0, 0]));
      const snapshot = accumulator.snapshot(3, 3);
      expect(accumulator.pointCount).toBeLessThanOrEqual(2500);
      expect(snapshot.averageStableResidualMeters).toBeLessThan(0.06);
      assertFiniteArray(snapshot.positions);
    }
  });
});
