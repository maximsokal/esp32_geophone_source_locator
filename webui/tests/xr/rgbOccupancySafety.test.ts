import { afterEach, describe, expect, it } from 'vitest';

import {
  DepthPointCloudAccumulator,
  type CpuDepthInformationLike,
  type DepthSampleView,
} from '../../src/xr/depthPointCloud';
import type { RgbDepthGuidanceFrame } from '../../src/xr/rgbDepthGuidance';
import {
  resetXrGuidanceRegistry,
  setLatestRgbGuidance,
} from '../../src/xr/xrGuidanceRegistry';

const projection = new Float32Array([
  1, 0, 0, 0,
  0, 1, 0, 0,
  0, 0, 1, 0,
  0, 0, 0, 1,
]);
const identity = new Float32Array(projection);
const view: DepthSampleView = { projectionMatrix: projection, transform: { matrix: identity } };

function physicalPlane(depthMeters: number): CpuDepthInformationLike {
  return { width: 160, height: 90, getDepthInMeters: () => depthMeters };
}

function allForbiddenGuidance(now: number): RgbDepthGuidanceFrame {
  const width = 256;
  const height = 144;
  return {
    width,
    height,
    luma: new Uint8Array(width * height).fill(128),
    edgeStrength: new Uint8Array(width * height).fill(255),
    gradientX: new Int8Array(width * height).fill(127),
    gradientY: new Int8Array(width * height),
    capturedAtMs: now,
  };
}

afterEach(() => resetXrGuidanceRegistry());

describe('RGB guidance / occupancy safety boundary', () => {
  it('can suppress every surface-fusion sample without suppressing physical FREE-ray evidence', () => {
    const accumulator = new DepthPointCloudAccumulator();
    const depth = physicalPlane(1.5);
    setLatestRgbGuidance(allForbiddenGuidance(performance.now()));

    for (let keyframeId = 1; keyframeId <= 4; keyframeId += 1) {
      accumulator.sampleDepthFrame(depth, view, {
        keyframeId,
        evidenceKeyframe: true,
        frameWeight: 1,
        cameraProjectPosition: [0, 0, 0],
      });
    }

    const snapshot = accumulator.snapshot(4, 4);
    // Strong visual discontinuity was allowed to veto SURFACE surfels.
    expect(snapshot.pointCount).toBe(0);
    expect(snapshot.rawPointCount).toBe(0);

    // Occupancy saw the untouched physical depth object, not the RGB-vetoed
    // surface wrapper. Repeated physical rays therefore still certify air and
    // measured surface cells independently.
    expect(snapshot.knownFreeVoxelCount).toBeGreaterThan(0);
    expect(snapshot.occupiedSurfaceVoxelCount).toBeGreaterThan(0);
    expect(snapshot.freeAirPositions.length).toBeGreaterThan(0);
    expect(snapshot.occupiedSurfacePositions.length).toBeGreaterThan(0);
  });
});
