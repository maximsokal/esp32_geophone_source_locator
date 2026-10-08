import { describe, expect, it, vi } from 'vitest';
import {
  DepthPointCloudAccumulator,
  type CpuDepthInformationLike,
  type DepthFusionFrameContext,
  type DepthSampleView,
} from '../../src/xr/depthPointCloudV2';
import { surfaceRenderConfidence } from '../../src/xr/surfelEvidence';
import {
  ARCORE_NOISE, nearestPolygon, orbitPoses, rectangle, scanScene, stepScene, tiltedPlaneScene,
  type SyntheticScene,
} from './support/syntheticDepthScene';

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

function syntheticCoverageAccumulator(cellCount: number, representativesPerCell: number): DepthPointCloudAccumulator {
  const accumulator = new DepthPointCloudAccumulator();
  const internal = accumulator as unknown as {
    coverageCells: Map<string, any>; representativeCount: number; promotedRepresentativeCount: number; integrationKeyframes: number;
  };
  internal.integrationKeyframes = 2;
  for (let cellIndex = 0; cellIndex < cellCount; cellIndex += 1) {
    const representatives = new Map<string, any>();
    for (let pointIndex = 0; pointIndex < representativesPerCell; pointIndex += 1) {
      const id = `${cellIndex}:${pointIndex}`;
      representatives.set(id, {
        id, x: cellIndex + pointIndex * 0.01, y: pointIndex * 0.02, z: cellIndex * 0.03,
        normalX: 0, normalY: 1, normalZ: 0, normalConfidence: 0.95, normalKnown: true,
        weight: 5, acceptedSamples: 8, independentViews: 4, viewCodesPacked: 0, viewCodeCount: 4,
        lastProcessedKeyframe: 2, lastEvidenceKeyframe: 2, lastSeenEpoch: 2, lastEvidenceEpoch: 2,
        varianceX: 0.000001, varianceY: 0.000001, varianceZ: 0.000001, residualEwma: 0.001,
        viewDirX: 0, viewDirY: 0, viewDirZ: -1, maxViewAngleRadians: Math.PI / 4,
        contradictionWeight: 0, promoted: true, priority: 0.5 + (pointIndex % 6) * 0.1,
        voxelKey: id, voxelIndexKey: id, coverageKey: String(cellIndex),
      });
    }
    internal.coverageCells.set(String(cellIndex), {
      representatives, lastEvidenceKeyframe: 2, evidenceKeyframes: 2, firstCameraX: 0, firstCameraY: 0, firstCameraZ: 0,
      hasFirstCamera: true, maxBaselineMeters: 1, retentionComplexityRevision: 0, retentionComplexityCachedRevision: 0,
      retentionComplexityCachedScore: 0, facesReadyCachedRevision: 0, facesReadyCachedValue: false,
    });
    internal.representativeCount += representativesPerCell;
    internal.promotedRepresentativeCount += representativesPerCell;
  }
  return accumulator;
}

function oldCoverageSelection(accumulator: DepthPointCloudAccumulator) {
  const internal = accumulator as unknown as { coverageCells: Map<string, any>; cellQuality: (cell: any) => { confidence: number; visible: boolean } };
  const cells = [...internal.coverageCells.values()];
  let visibleCoverageCells = 0;
  for (const cell of cells) if (internal.cellQuality(cell).visible) visibleCoverageCells += 1;
  const perCellBudget = Math.max(1, Math.min(12, Math.floor(12000 / Math.max(1, visibleCoverageCells))));
  const positions = new Float32Array(12000 * 3); const confidence = new Float32Array(12000); let pointCount = 0;
  const quality = (point: any): number => {
    const residual = Math.max(Math.sqrt(Math.max(0, (point.varianceX + point.varianceY + point.varianceZ) / 3)), point.residualEwma * 0.55);
    return surfaceRenderConfidence({ promoted: point.promoted, independentViews: point.independentViews, weight: point.weight,
      residualMeters: residual, maxViewAngleRadians: point.maxViewAngleRadians, contradictionWeight: point.contradictionWeight,
      normalConfidence: point.normalConfidence });
  };
  const score = (point: any): number => point.priority / (Math.max(0.06, quality(point)) * (1 + Math.min(point.independentViews, 4) * 0.35));
  for (const cell of cells) {
    const cellQuality = internal.cellQuality(cell); if (!cellQuality.visible) continue;
    const ranked = [...cell.representatives.values()].sort((a, b) => score(a) - score(b));
    const take = Math.min(Math.max(1, Math.round(perCellBudget * (0.45 + 0.55 * Math.max(0.2, cellQuality.confidence)))), ranked.length);
    for (let index = 0; index < take && pointCount < 12000; index += 1) {
      const point = ranked[index];const offset = pointCount * 3;
      positions[offset] = point.x;positions[offset + 1] = point.z;positions[offset + 2] = -point.y;
      confidence[pointCount] = quality(point);pointCount += 1;
    }
  }
  return { positions: positions.subarray(0, pointCount * 3), confidence: confidence.subarray(0, pointCount), pointCount };
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

describe('budgeted live coverage snapshots (MAX-409)', () => {
  it('matches the old stable-score selection and reuses exact-sized views under pressure', () => {
    const accumulator = syntheticCoverageAccumulator(1000, 12);
    const internal = accumulator as unknown as { representativeCount: number };
    const scientificPointCount = internal.representativeCount;
    const old = oldCoverageSelection(accumulator);
    const full = accumulator.liveCoverageSnapshot(12000);
    expect(full.pointCount).toBe(12000);
    expect(full.positions).toEqual(old.positions);
    expect(full.confidence).toEqual(old.confidence);

    const positionBuffer = full.positions.buffer;const confidenceBuffer = full.confidence.buffer;
    const sortSpy = vi.spyOn(Array.prototype, 'sort');
    try {
      const reduced = accumulator.liveCoverageSnapshot(4800);
      expect(reduced.pointCount).toBe(4800);
      expect(reduced.positions.length).toBe(reduced.pointCount * 3);
      expect(reduced.confidence.length).toBe(reduced.pointCount);
      expect(reduced.positions.buffer).toBe(positionBuffer);
      expect(reduced.confidence.buffer).toBe(confidenceBuffer);
      expect(sortSpy).not.toHaveBeenCalled();
    } finally { sortSpy.mockRestore(); }
    expect(internal.representativeCount).toBe(scientificPointCount);
  });
});

describe('structural guide snapshots (MAX-409)', () => {
  it('preserves eligible representative order and values with exact-sized allocations under pressure', () => {
    const accumulator = new DepthPointCloudAccumulator();
    const internal = accumulator as unknown as {
      representatives: Map<string, any>;
      integrationKeyframes: number;
    };
    internal.integrationKeyframes = 17;

    for (let index = 0; index < 60000; index += 1) {
      const promoted = index % 1000 !== 0;
      internal.representatives.set(String(index), {
        id: String(index),
        x: index * 0.001,
        y: index * 0.002,
        z: -index * 0.001,
        normalX: 0.1,
        normalY: 0.9,
        normalZ: -0.2,
        normalConfidence: 0.8,
        normalKnown: index % 5 === 0,
        independentViews: 3 + index % 3,
        weight: 4 + index % 2,
        varianceX: 0.000001 * (1 + index % 4),
        varianceY: 0.000002 * (1 + index % 3),
        varianceZ: 0.000003 * (1 + index % 2),
        residualEwma: 0.001 + (index % 7) * 0.0001,
        maxViewAngleRadians: Math.PI / 5,
        contradictionWeight: (index % 4) * 0.03,
        promoted,
      });
    }

    const eligible = [...internal.representatives.values()].filter((point) => point.promoted && point.normalKnown);
    const expectedPositions = new Float32Array(eligible.length * 3);
    const expectedNormals = new Float32Array(eligible.length * 3);
    const expectedConfidence = new Float32Array(eligible.length);
    eligible.forEach((point, index) => {
      const offset = index * 3;
      expectedPositions[offset] = point.x;
      expectedPositions[offset + 1] = point.y;
      expectedPositions[offset + 2] = point.z;
      expectedNormals[offset] = point.normalX;
      expectedNormals[offset + 1] = point.normalY;
      expectedNormals[offset + 2] = point.normalZ;
      const residual = Math.max(
        Math.sqrt(Math.max(0, (point.varianceX + point.varianceY + point.varianceZ) / 3)),
        point.residualEwma * 0.55,
      );
      expectedConfidence[index] = surfaceRenderConfidence({
        promoted: point.promoted,
        independentViews: point.independentViews,
        weight: point.weight,
        residualMeters: residual,
        maxViewAngleRadians: point.maxViewAngleRadians,
        contradictionWeight: point.contradictionWeight,
        normalConfidence: point.normalConfidence,
      });
    });

    const nativeFloat32Array = Float32Array;
    const allocatedLengths: number[] = [];
    vi.stubGlobal('Float32Array', class extends nativeFloat32Array {
      constructor(length: number) {
        super(length);
        allocatedLengths.push(length);
      }
    });
    try {
      const snapshot = accumulator.structuralGuideSnapshot();
      expect(snapshot.pointCount).toBe(eligible.length);
      expect(snapshot.keyframeCount).toBe(17);
      expect(Array.from(snapshot.positions)).toEqual(Array.from(expectedPositions));
      expect(Array.from(snapshot.normals)).toEqual(Array.from(expectedNormals));
      expect(Array.from(snapshot.confidence)).toEqual(Array.from(expectedConfidence));
      expect(allocatedLengths).toEqual([eligible.length * 3, eligible.length * 3, eligible.length]);
      expect(snapshot.positions.byteLength).toBe(eligible.length * 3 * Float32Array.BYTES_PER_ELEMENT);
      expect(snapshot.normals.byteLength).toBe(eligible.length * 3 * Float32Array.BYTES_PER_ELEMENT);
      expect(snapshot.confidence.byteLength).toBe(eligible.length * Float32Array.BYTES_PER_ELEMENT);
    } finally {
      vi.unstubAllGlobals();
    }
  });
});

describe('surface reconstruction behaviour', () => {
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

  // MAX-260: the former opposite-view fusion case reused one depth map with a
  // camera behind the surface; opposite-facing observations are now separate
  // oriented surfaces (harness test in signedNormals.test.ts).
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

/*
 * MAX-251: the three cases below used to feed ONE depth map with an identity
 * transform for every "camera", so no parallax existed. They now scan analytic
 * scenes from real look-at poses (tests/xr/support/syntheticDepthScene.ts).
 */
describe('multi-view behaviour on geometrically consistent scans', () => {
  it('promotes surfaces only after a real side baseline creates independent view evidence', () => {
    const scene = tiltedPlaneScene(0, 0, 1.2);
    const poses = orbitPoses(scene.focus, 1.5, 70, 0, 120, 3);
    const single = new DepthPointCloudAccumulator();
    scanScene(single, scene, [poses[0]], { noise: ARCORE_NOISE });
    expect(single.promotedCount).toBe(0);
    const moved = new DepthPointCloudAccumulator();
    scanScene(moved, scene, poses, { noise: ARCORE_NOISE });
    expect(moved.promotedCount).toBeGreaterThan(0);
    const snapshot = moved.snapshot(3, 3);
    expect(snapshot.pointCount).toBeGreaterThan(0);
    expect(Math.max(...snapshot.confidence)).toBeGreaterThan(0.5);
  });

  it('preserves both sides of a sharp depth step', () => {
    const scene = stepScene(0.25, 1.0);
    const accumulator = new DepthPointCloudAccumulator();
    scanScene(accumulator, scene, orbitPoses(scene.focus, 1.4, 60, 150, 210, 5), { noise: ARCORE_NOISE });
    const snapshot = accumulator.snapshot(5, 5);
    const onFace = { lower: 0, upper: 0 };
    for (let i = 0; i < snapshot.pointCount; i += 1) {
      const hit = nearestPolygon(scene, [snapshot.positions[i * 3], snapshot.positions[i * 3 + 1], snapshot.positions[i * 3 + 2]]);
      // 1.5 cm = ~3 sigma of the depth noise at 1.4-1.7 m.
      if (hit.distance < 0.015 && (hit.polygon.label === 'lower' || hit.polygon.label === 'upper')) onFace[hit.polygon.label] += 1;
    }
    expect(onFace.lower).toBeGreaterThan(30);
    expect(onFace.upper).toBeGreaterThan(30);
  });

  it('retains a narrow protrusion about 1.5 depth pixels wide', () => {
    // 2.5 cm wide bar standing 0.3 m above a floor; at 1.2 m one depth pixel is ~1.5 cm.
    const bar = 0.0125;
    const scene: SyntheticScene = {
      name: 'narrow-bar',
      polygons: [
        rectangle([0, 0, 0], [0.6, 0, 0], [0, 0.6, 0], 'floor'),
        rectangle([0, 0, 0.3], [bar, 0, 0], [0, 0.25, 0], 'bar-top'),
        rectangle([bar, 0, 0.15], [0, 0.25, 0], [0, 0, 0.15], 'bar+x'),
        rectangle([-bar, 0, 0.15], [0, 0, 0.15], [0, 0.25, 0], 'bar-x'),
      ],
      creases: [], corners: [], focus: [0, 0, 0.15],
    };
    const accumulator = new DepthPointCloudAccumulator();
    scanScene(accumulator, scene, orbitPoses(scene.focus, 1.2, 65, 0, 300, 6), { noise: ARCORE_NOISE });
    const snapshot = accumulator.snapshot(6, 6);
    let barTop = 0; let floor = 0;
    for (let i = 0; i < snapshot.pointCount; i += 1) {
      const z = snapshot.positions[i * 3 + 2];
      if (z > 0.27) barTop += 1;
      if (z < 0.03) floor += 1;
    }
    expect(barTop).toBeGreaterThan(3);
    expect(floor).toBeGreaterThan(100);
  });
});
