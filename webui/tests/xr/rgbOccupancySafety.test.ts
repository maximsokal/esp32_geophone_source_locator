import { afterEach, describe, expect, it } from 'vitest';

import {
  DepthPointCloudAccumulator,
  type CpuDepthInformationLike,
  type DepthSampleView,
  type MonocularSurfacePrior,
  type ProjectPoint,
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

function guidance(
  now: number,
  forbidden: boolean,
  worldFromView?: Float32Array,
): RgbDepthGuidanceFrame {
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
    worldFromView,
    capturedAtMs: now,
  };
}

class RejectPostScanSurfacePrior implements MonocularSurfacePrior {
  readonly isEnabled = true;
  stageCalls = 0;
  agreementCalls = 0;
  private staged = false;

  reset(): void { this.staged = false; }

  stagePostScan(): boolean {
    this.stageCalls += 1;
    this.staged = true;
    return true;
  }

  postScanSurfaceAgreement(_position: ProjectPoint): { available: boolean; accepted: boolean } {
    this.agreementCalls += 1;
    return { available: this.staged, accepted: false };
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

function freshForWholeTest(): number {
  // Coverage/instrumented runs can make synthetic fusion much slower than the
  // normal 450 ms RGB freshness window. A future fixture timestamp keeps the
  // guidance intentionally fresh without weakening the production TTL.
  return performance.now() + 60_000;
}

afterEach(() => resetXrGuidanceRegistry());

describe('RGB / AI surface guidance versus occupancy safety boundary', () => {
  it('can suppress every RGB surface-fusion sample without suppressing physical FREE-ray evidence', () => {
    const accumulator = new DepthPointCloudAccumulator();
    const depth = physicalPlane(1.5);
    setLatestRgbGuidance(guidance(freshForWholeTest(), true));

    integrateRepeatedPhysicalPlane(accumulator, depth);

    const snapshot = accumulator.snapshot(4, 4);
    expect(snapshot.pointCount).toBe(0);
    expect(snapshot.rawPointCount).toBe(0);

    expect(snapshot.knownFreeVoxelCount).toBeGreaterThan(0);
    expect(snapshot.occupiedSurfaceVoxelCount).toBeGreaterThan(0);
    expect(snapshot.freeAirPositions.length).toBeGreaterThan(0);
    expect(snapshot.occupiedSurfacePositions.length).toBeGreaterThan(0);
  });

  it('stages optional AI during live XR without letting the model touch live physical surface fusion or occupancy', () => {
    const prior = new RejectPostScanSurfacePrior();
    const accumulator = new DepthPointCloudAccumulator(undefined, undefined, undefined, prior);
    const depth = physicalPlane(1.5);
    setLatestRgbGuidance(guidance(freshForWholeTest(), false));

    integrateRepeatedPhysicalPlane(accumulator, depth);

    const snapshot = accumulator.snapshot(4, 4);
    expect(prior.stageCalls).toBeGreaterThan(0);
    expect(snapshot.rawPointCount).toBeGreaterThan(0);
    expect(snapshot.pointCount).toBeGreaterThan(0);

    // Diploma safety invariant: model output has no path into the live fusion or
    // occupancy integrator. Only repeated physical ARCore rays establish FREE.
    expect(snapshot.knownFreeVoxelCount).toBeGreaterThan(0);
    expect(snapshot.occupiedSurfaceVoxelCount).toBeGreaterThan(0);
    expect(snapshot.freeAirPositions.length).toBeGreaterThan(0);
    expect(snapshot.occupiedSurfacePositions.length).toBeGreaterThan(0);
  });

  it('does not even stage AI from a time-fresh RGB frame after the camera moved beyond the capture-pose gate', () => {
    const prior = new RejectPostScanSurfacePrior();
    const accumulator = new DepthPointCloudAccumulator(undefined, undefined, undefined, prior);
    const depth = physicalPlane(1.5);
    const oldPose = new Float32Array(identity);
    oldPose[12] = 0.20;
    setLatestRgbGuidance(guidance(freshForWholeTest(), true, oldPose));

    integrateRepeatedPhysicalPlane(accumulator, depth);

    const snapshot = accumulator.snapshot(4, 4);
    expect(prior.stageCalls).toBe(0);
    expect(snapshot.rawPointCount).toBeGreaterThan(0);
    expect(snapshot.knownFreeVoxelCount).toBeGreaterThan(0);
    expect(snapshot.occupiedSurfaceVoxelCount).toBeGreaterThan(0);
  });
});