import { afterEach, describe, expect, it } from 'vitest';

import {
  DepthPointCloudAccumulator,
  type CpuDepthInformationLike,
  type DepthSampleView,
  type MonocularSurfacePrior,
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

function guidance(now: number, forbidden: boolean): RgbDepthGuidanceFrame {
  const width = 256;
  const height = 144;
  return {
    width,
    height,
    luma: new Uint8Array(width * height).fill(128),
    edgeStrength: forbidden
      ? new Uint8Array(width * height).fill(255)
      : new Uint8Array(width * height),
    gradientX: forbidden
      ? new Int8Array(width * height).fill(127)
      : new Int8Array(width * height),
    gradientY: new Int8Array(width * height),
    capturedAtMs: now,
  };
}

class RejectEverySurfacePrior implements MonocularSurfacePrior {
  readonly isEnabled = true;
  updateCalls = 0;
  agreementCalls = 0;

  reset(): void {}

  async maybeUpdate(): Promise<boolean> {
    this.updateCalls += 1;
    return false;
  }

  surfaceAgreement(): { accepted: boolean } {
    this.agreementCalls += 1;
    return { accepted: false };
  }
}

function integrateRepeatedPhysicalPlane(accumulator: DepthPointCloudAccumulator, depth: CpuDepthInformationLike): void {
  for (let keyframeId = 1; keyframeId <= 4; keyframeId += 1) {
    accumulator.sampleDepthFrame(depth, view, {
      keyframeId,
      evidenceKeyframe: true,
      frameWeight: 1,
      cameraProjectPosition: [0, 0, 0],
    });
  }
}

afterEach(() => resetXrGuidanceRegistry());

describe('RGB / AI surface guidance versus occupancy safety boundary', () => {
  it('can suppress every RGB surface-fusion sample without suppressing physical FREE-ray evidence', () => {
    const accumulator = new DepthPointCloudAccumulator();
    const depth = physicalPlane(1.5);
    setLatestRgbGuidance(guidance(performance.now(), true));

    integrateRepeatedPhysicalPlane(accumulator, depth);

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

  it('can reject every AI surface sample while raw ARCore depth still certifies FREE and OCCUPIED voxels', () => {
    const prior = new RejectEverySurfacePrior();
    const accumulator = new DepthPointCloudAccumulator(undefined, undefined, undefined, prior);
    const depth = physicalPlane(1.5);
    // Flat RGB keeps the RGB path permissive, so the only surface veto here is
    // the injected model prior.
    setLatestRgbGuidance(guidance(performance.now(), false));

    integrateRepeatedPhysicalPlane(accumulator, depth);

    const snapshot = accumulator.snapshot(4, 4);
    expect(prior.updateCalls).toBeGreaterThan(0);
    expect(prior.agreementCalls).toBeGreaterThan(0);
    expect(snapshot.pointCount).toBe(0);
    expect(snapshot.rawPointCount).toBe(0);

    // This is the diploma safety invariant: model output has no path into the
    // occupancy integrator. Only repeated physical ARCore rays can establish
    // FREE air or measured OCCUPIED surface voxels.
    expect(snapshot.knownFreeVoxelCount).toBeGreaterThan(0);
    expect(snapshot.occupiedSurfaceVoxelCount).toBeGreaterThan(0);
    expect(snapshot.freeAirPositions.length).toBeGreaterThan(0);
    expect(snapshot.occupiedSurfacePositions.length).toBeGreaterThan(0);
  });
});
