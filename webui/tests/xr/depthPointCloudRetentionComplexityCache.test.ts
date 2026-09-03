import { describe, expect, it, vi } from 'vitest';

const retentionComplexityCalls = vi.hoisted(() => vi.fn());

vi.mock('../../src/xr/surfaceFeatureDensity', async (importOriginal) => {
  const actual = await importOriginal<typeof import('../../src/xr/surfaceFeatureDensity')>();
  return {
    ...actual,
    estimateSurfaceFeatureComplexity(
      evidence: Parameters<typeof actual.estimateSurfaceFeatureComplexity>[0],
    ) {
      retentionComplexityCalls();
      return actual.estimateSurfaceFeatureComplexity(evidence);
    },
  };
});

import {
  DepthPointCloudAccumulator,
  type CpuDepthInformationLike,
  type DepthFusionFrameContext,
  type DepthSampleView,
} from '../../src/xr/depthPointCloudV2';

interface InternalRepresentative {
  id: string;
  x: number;
  y: number;
  z: number;
  normalX: number;
  normalY: number;
  normalZ: number;
  normalConfidence: number;
  weight: number;
  acceptedSamples: number;
  independentViews: number;
  viewCodesPacked: number;
  viewCodeCount: number;
  lastProcessedKeyframe: number;
  lastEvidenceKeyframe: number;
  lastSeenEpoch: number;
  lastEvidenceEpoch: number;
  varianceX: number;
  varianceY: number;
  varianceZ: number;
  residualEwma: number;
  viewDirX: number;
  viewDirY: number;
  viewDirZ: number;
  maxViewAngleRadians: number;
  contradictionWeight: number;
  promoted: boolean;
  priority: number;
  voxelKey: string;
  voxelIndexKey: number | string;
  coverageKey: string;
}

interface InternalCoverageCell {
  representatives: Map<string, InternalRepresentative>;
  retentionComplexityRevision: number;
  retentionComplexityCachedRevision: number;
  retentionComplexityCachedScore: number;
}

interface InternalAccumulator {
  coverageCells: Map<string, InternalCoverageCell>;
  cellFeatureComplexity(cell: InternalCoverageCell, candidate?: InternalRepresentative): number;
  createCoverageCell(): InternalCoverageCell;
  insertCandidate(cell: InternalCoverageCell, candidate: InternalRepresentative): boolean;
  removeRepresentative(point: InternalRepresentative): void;
  fuseRepresentative(
    point: InternalRepresentative,
    x: number,
    y: number,
    z: number,
    depthMeters: number,
    observationWeight: number,
    normalEstimate: { normal: readonly [number, number, number]; confidence: number } | null,
    context: DepthFusionFrameContext,
    residual: number,
  ): void;
  registerContradictionIfApplicable(
    point: InternalRepresentative,
    x: number,
    y: number,
    z: number,
    residual: number,
    depthMeters: number,
    context: DepthFusionFrameContext,
  ): void;
}

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

function internals(accumulator: DepthPointCloudAccumulator): InternalAccumulator {
  return accumulator as unknown as InternalAccumulator;
}

function largestCell(accumulator: InternalAccumulator): InternalCoverageCell {
  const cells = [...accumulator.coverageCells.values()];
  expect(cells.length).toBeGreaterThan(0);
  return cells.reduce((left, right) =>
    right.representatives.size > left.representatives.size ? right : left
  );
}

function originalVoxelCenter(point: InternalRepresentative, voxelSizeMeters: number): readonly [number, number, number] {
  const [x, y, z] = point.voxelKey.split(':').map(Number);
  expect([x, y, z].every(Number.isFinite)).toBe(true);
  return [
    (x + 0.5) * voxelSizeMeters,
    (y + 0.5) * voxelSizeMeters,
    (z + 0.5) * voxelSizeMeters,
  ];
}

async function uncachedScore(cell: InternalCoverageCell): Promise<number> {
  const actual = await vi.importActual<typeof import('../../src/xr/surfaceFeatureDensity')>(
    '../../src/xr/surfaceFeatureDensity',
  );
  return actual.estimateSurfaceFeatureComplexity(cell.representatives.values()).score;
}

describe('retention feature-complexity revision cache', () => {
  it('reuses an unchanged cell and invalidates after fusion, insert and remove mutations', async () => {
    const accumulator = new DepthPointCloudAccumulator();
    accumulator.sampleDepthFrame(depthPlane(() => 1.5), view, context(1, true));
    const internal = internals(accumulator);
    const cell = largestCell(internal);
    const point = [...cell.representatives.values()][0];
    expect(point).toBeDefined();

    retentionComplexityCalls.mockClear();
    const initialExpected = await uncachedScore(cell);
    const initial = internal.cellFeatureComplexity(cell);
    expect(initial).toBeCloseTo(initialExpected, 12);
    expect(retentionComplexityCalls).toHaveBeenCalledTimes(1);
    expect(internal.cellFeatureComplexity(cell)).toBe(initial);
    expect(retentionComplexityCalls).toHaveBeenCalledTimes(1);

    const fusionRevision = cell.retentionComplexityRevision;
    internal.fuseRepresentative(
      point,
      point.x + 0.001,
      point.y,
      point.z,
      1.5,
      0.45,
      { normal: [0, 1, 0], confidence: 0.9 },
      context(2, true, [0.45, 0, 0]),
      0.001,
    );
    expect(cell.retentionComplexityRevision).toBeGreaterThan(fusionRevision);
    const fusedExpected = await uncachedScore(cell);
    expect(internal.cellFeatureComplexity(cell)).toBeCloseTo(fusedExpected, 12);
    expect(retentionComplexityCalls).toHaveBeenCalledTimes(2);
    internal.cellFeatureComplexity(cell);
    expect(retentionComplexityCalls).toHaveBeenCalledTimes(2);

    const isolatedKey = 'retention-cache-test-cell';
    const isolatedCell = internal.createCoverageCell();
    internal.coverageCells.set(isolatedKey, isolatedCell);
    const candidate: InternalRepresentative = {
      ...point,
      id: 'retention-cache-test-point',
      x: 10,
      y: 10,
      z: 10,
      coverageKey: isolatedKey,
      voxelKey: 'retention-cache-test-voxel',
      voxelIndexKey: 'retention-cache-test-index',
      promoted: false,
      independentViews: 1,
      normalX: 0,
      normalY: 1,
      normalZ: 0,
      normalConfidence: 0.9,
      priority: 1e-9,
    };

    const insertRevision = isolatedCell.retentionComplexityRevision;
    expect(internal.insertCandidate(isolatedCell, candidate)).toBe(true);
    expect(isolatedCell.retentionComplexityRevision).toBeGreaterThan(insertRevision);
    const insertedExpected = await uncachedScore(isolatedCell);
    expect(internal.cellFeatureComplexity(isolatedCell)).toBeCloseTo(insertedExpected, 12);
    expect(retentionComplexityCalls).toHaveBeenCalledTimes(3);
    internal.cellFeatureComplexity(isolatedCell);
    expect(retentionComplexityCalls).toHaveBeenCalledTimes(3);

    const removeRevision = isolatedCell.retentionComplexityRevision;
    internal.removeRepresentative(candidate);
    expect(isolatedCell.retentionComplexityRevision).toBeGreaterThan(removeRevision);
    const removedExpected = await uncachedScore(isolatedCell);
    expect(internal.cellFeatureComplexity(isolatedCell)).toBeCloseTo(removedExpected, 12);
    expect(retentionComplexityCalls).toHaveBeenCalledTimes(4);
  });

  it('invalidates a cached mature cell when contradiction demotes a promoted surfel', async () => {
    const accumulator = new DepthPointCloudAccumulator();
    const flat = depthPlane(() => 1.5);
    accumulator.sampleDepthFrame(flat, view, context(1, true, [0, 0, 0]));
    accumulator.sampleDepthFrame(flat, view, context(2, true, [0.45, 0, 0]));
    expect(accumulator.promotedCount).toBeGreaterThan(0);

    const internal = internals(accumulator);
    const promotedEntry = [...internal.coverageCells.values()]
      .map((cell) => ({
        cell,
        point: [...cell.representatives.values()].find((candidate) => candidate.promoted),
      }))
      .find((entry) => entry.point !== undefined);
    expect(promotedEntry?.point).toBeDefined();
    const { cell, point } = promotedEntry! as { cell: InternalCoverageCell; point: InternalRepresentative };

    retentionComplexityCalls.mockClear();
    const beforeExpected = await uncachedScore(cell);
    expect(internal.cellFeatureComplexity(cell)).toBeCloseTo(beforeExpected, 12);
    internal.cellFeatureComplexity(cell);
    expect(retentionComplexityCalls).toHaveBeenCalledTimes(1);

    // contradictionWeight is not an input to feature complexity, so changing it
    // here does not itself invalidate the cache. The demotion path below must.
    point.contradictionWeight = 2.6;
    const revisionBeforeDemotion = cell.retentionComplexityRevision;
    const [sameVoxelX, sameVoxelY, sameVoxelZ] = originalVoxelCenter(point, accumulator.voxelSize);
    internal.registerContradictionIfApplicable(
      point,
      sameVoxelX,
      sameVoxelY,
      sameVoxelZ,
      0.5,
      1.5,
      context(point.lastProcessedKeyframe + 1, true, [0.45, 0, 0]),
    );
    expect(point.promoted).toBe(false);
    expect(cell.retentionComplexityRevision).toBeGreaterThan(revisionBeforeDemotion);

    const afterExpected = await uncachedScore(cell);
    expect(internal.cellFeatureComplexity(cell)).toBeCloseTo(afterExpected, 12);
    expect(retentionComplexityCalls).toHaveBeenCalledTimes(2);
    internal.cellFeatureComplexity(cell);
    expect(retentionComplexityCalls).toHaveBeenCalledTimes(2);
  });
});
