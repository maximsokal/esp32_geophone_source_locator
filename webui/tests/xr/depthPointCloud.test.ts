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

describe('DepthPointCloudAccumulator v2 regressions', () => {
  it('does not promote geometry just because global keyframe ids keep increasing', () => {
    const accumulator = new DepthPointCloudAccumulator();
    const flat = depthPlane(() => 1.5);
    accumulator.sampleDepthFrame(flat, view, context(1, true));
    const created = accumulator.pointCount;
    expect(created).toBeGreaterThan(100);

    for (let keyframeId = 2; keyframeId <= 12; keyframeId += 1) {
      accumulator.sampleDepthFrame(flat, view, context(keyframeId, true));
    }

    expect(accumulator.promotedCount).toBe(0);
    expect(accumulator.pointCount).toBeLessThanOrEqual(created * 2);
  });

  it('promotes surfaces after a real side baseline creates independent view evidence', () => {
    const accumulator = new DepthPointCloudAccumulator();
    const flat = depthPlane(() => 1.5);
    accumulator.sampleDepthFrame(flat, view, context(1, true, [0, 0, 0]));
    accumulator.sampleDepthFrame(flat, view, context(2, true, [0.45, 0, 0]));
    expect(accumulator.promotedCount).toBeGreaterThan(0);
  });

  it('allows polish frames to refine matched surfaces but never create new geometry', () => {
    const accumulator = new DepthPointCloudAccumulator();
    const flat = depthPlane(() => 1.5);
    accumulator.sampleDepthFrame(flat, view, context(1, true, [0, 0, 0]));
    accumulator.sampleDepthFrame(flat, view, context(2, true, [0.45, 0, 0]));
    const before = accumulator.pointCount;

    for (let index = 0; index < 8; index += 1) {
      accumulator.sampleDepthFrame(flat, view, context(2, false, [0.45, 0, 0], 0.15));
    }
    expect(accumulator.pointCount).toBe(before);
  });

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

  it('self-polishes an already promoted noisy plane toward later consistent depth', () => {
    const accumulator = new DepthPointCloudAccumulator();
    const noisy = depthPlane(() => 1.53);
    const truth = depthPlane(() => 1.5);
    accumulator.sampleDepthFrame(noisy, view, context(1, true, [0, 0, 0]));
    accumulator.sampleDepthFrame(noisy, view, context(2, true, [0.45, 0, 0]));

    const before = accumulator.snapshot(2, 2);
    const meanDepth = (positions: Float32Array): number => {
      let total = 0;
      for (let index = 1; index < positions.length; index += 3) total += positions[index];
      return total / Math.max(1, positions.length / 3);
    };
    const beforeError = Math.abs(meanDepth(before.positions) - 1.5);

    for (let index = 0; index < 24; index += 1) {
      accumulator.sampleDepthFrame(truth, view, context(2, false, [0.45, 0, 0], 0.18));
    }

    const after = accumulator.snapshot(26, 26);
    const afterError = Math.abs(meanDepth(after.positions) - 1.5);
    expect(after.pointCount).toBe(before.pointCount);
    expect(afterError).toBeLessThan(beforeError * 0.8);
  });

  it('eventually retains a narrow protrusion that is smaller than one base sampling stratum', () => {
    const accumulator = new DepthPointCloudAccumulator();
    const protrusion = depthPlane((u, v) =>
      u > 0.488 && u < 0.512 && v > 0.25 && v < 0.75 ? 1.0 : 1.5,
    );
    const cameras: Array<readonly [number, number, number]> = [
      [0, 0, 0],
      [0.45, 0, 0],
      [-0.45, 0, 0],
      [0.30, 0.20, 0],
      [-0.30, 0.20, 0],
    ];

    cameras.forEach((camera, index) => {
      accumulator.sampleDepthFrame(protrusion, view, context(index + 1, true, camera));
    });

    const snapshot = accumulator.snapshot(cameras.length, cameras.length);
    let protrusionPoints = 0;
    let wallPoints = 0;
    for (let index = 1; index < snapshot.positions.length; index += 3) {
      const depth = snapshot.positions[index];
      if (depth < 1.2) protrusionPoints += 1;
      if (depth > 1.35) wallPoints += 1;
    }
    expect(protrusionPoints).toBeGreaterThan(5);
    expect(wallPoints).toBeGreaterThan(100);
  });

  it('keeps the retained map inside its hard memory budget', () => {
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
});
