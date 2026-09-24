import { describe, expect, it } from 'vitest';

import type { StructuralGuidePointCloudSnapshot } from '../../src/xr/depthPointCloudV2';
import { DepthPointCloudAccumulator } from '../../src/xr/depthPointCloudV2';
import { computeScanCapabilityReadiness, computeStructuralFaceCapability, structuralFaceDensityTarget } from '../../src/xr/scanCapabilityReadiness';
import type { SurfaceComponent } from '../../src/xr/surfaceComponents';
import { compactScanReadinessLabel } from '../../src/xr/scanReadinessUi';
import { prepareStructuralGuideReadiness } from '../../src/xr/structuralGuideReadiness';
import { StructuralGuidePipeline } from '../../src/xr/structuralGuidePipeline';
import {
  ARCORE_NOISE, boxCornerScene, orbitPoses, renderDepth, scanScene, tiltedPlaneScene,
  type CameraPose, type SyntheticScene, type Vec3,
} from './support/syntheticDepthScene';

/*
 * MAX-254: "enough points" must mean enough for an EDGE / CORNER: several
 * adjacent non-parallel faces, each sampled at a density the detector can use
 * (points per planar area, derived from the voxel size), never one face.
 */

type V3 = [number, number, number];

function rng(seed: number): () => number {
  let state = seed >>> 0;
  return () => { state = (Math.imul(state, 1664525) + 1013904223) >>> 0; return (state + 0.5) / 4294967296 - 0.5; };
}

interface Cloud { positions: number[]; normals: number[]; confidence: number[] }
const empty = (): Cloud => ({ positions: [], normals: [], confidence: [] });

/** Rectangle face from origin along unit u (length lu) and v (length lv), jittered grid sampling; normal u x v. */
function face(cloud: Cloud, origin: V3, u: V3, v: V3, lu: number, lv: number, spacing: number, next: () => number): void {
  const normal: V3 = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]];
  for (let i = 0; (i + 0.5) * spacing < lu; i += 1) for (let j = 0; (j + 0.5) * spacing < lv; j += 1) {
    const s = (i + 0.5 + 0.6 * next()) * spacing; const t = (j + 0.5 + 0.6 * next()) * spacing; const e = 0.003 * next();
    cloud.positions.push(origin[0] + u[0] * s + v[0] * t + normal[0] * e, origin[1] + u[1] * s + v[1] * t + normal[1] * e, origin[2] + u[2] * s + v[2] * t + normal[2] * e);
    cloud.normals.push(...normal);
    cloud.confidence.push(0.92);
  }
}

function snapshot(cloud: Cloud): StructuralGuidePointCloudSnapshot {
  return {
    positions: Float32Array.from(cloud.positions), normals: Float32Array.from(cloud.normals), confidence: Float32Array.from(cloud.confidence),
    pointCount: cloud.confidence.length, keyframeCount: 10, voxelSizeMeters: 0.04,
  };
}

const DENSE = 0.03;   // 1111 /m^2, far above the 216 /m^2 face target at 4 cm voxels
const FRAGMENTED = 0.07; // 204 /m^2: too sparse to even connect into one face
const X: V3 = [1, 0, 0]; const Y: V3 = [0, 1, 0]; const Z: V3 = [0, 0, 1];
const NX: V3 = [-1, 0, 0]; const NY: V3 = [0, -1, 0]; const NZ: V3 = [0, 0, -1];

/** Box corner at the origin: faces x=0, y=0, z=0 of [-L,0]^3 (normals +X, +Y, +Z), per-face spacing. */
function boxCorner(spacings: [number, number, number], size = 0.3, seed = 1, faces = 3): Cloud {
  const next = rng(seed); const c = empty();
  face(c, [0, 0, 0], NY, NZ, size, size, spacings[0], next);
  face(c, [0, 0, 0], NZ, NX, size, size, spacings[1], next);
  if (faces === 3) face(c, [0, 0, 0], NX, NY, size, size, spacings[2], next);
  return c;
}

function capability(cloud: Cloud) {
  const prepared = prepareStructuralGuideReadiness(snapshot(cloud));
  return { readiness: prepared.structuralReadiness, faces: prepared.surfaceReadiness.structuralFaces! };
}

describe('structural sufficiency scenario matrix (expected results from the scene, not the code)', () => {
  it('single dense plane: surface ready, never edge- or corner-ready, next step is another face', () => {
    const c = empty();
    face(c, [0, 0, 0], X, Y, 0.3, 0.3, DENSE, rng(3));
    const { readiness } = capability(c);
    expect(readiness.capability).toEqual({ surface: true, edge: false, corner: false });
    expect(readiness.ready).toBe(false);
    expect(readiness.limitingMetric).toBe('faces');
  });

  it('two dense faces meeting at 90 deg: edge-ready, not corner-ready', () => {
    const { readiness } = capability(boxCorner([DENSE, DENSE, DENSE], 0.3, 1, 2));
    expect(readiness.capability).toEqual({ surface: true, edge: true, corner: false });
    expect(readiness.ready).toBe(true);
  });

  it('dense box corner: corner-ready', () => {
    const { readiness } = capability(boxCorner([DENSE, DENSE, DENSE]));
    expect(readiness.capability).toEqual({ surface: true, edge: true, corner: true });
  });

  it('box corner whose third face is too sparse to connect: still edge-ready, never corner-ready', () => {
    const { readiness } = capability(boxCorner([DENSE, DENSE, FRAGMENTED]));
    expect(readiness.capability.edge).toBe(true);
    expect(readiness.capability.corner).toBe(false);
  });

  it('negative control: two parallel walls facing each other are never edge-ready', () => {
    const c = empty(); const next = rng(9);
    // 5 cm apart: close enough to count as touching, so only the angle gate can reject them.
    face(c, [0, 0, 0], Y, Z, 0.3, 0.3, DENSE, next);     // normal +X
    face(c, [0.05, 0, 0], Z, Y, 0.3, 0.3, DENSE, next);  // normal -X
    expect(capability(c).readiness.capability.edge).toBe(false);
  });

  it('negative control: perpendicular faces that do not touch (30 cm apart) are never edge-ready', () => {
    const c = empty(); const next = rng(11);
    face(c, [0, 0, 0], Y, Z, 0.3, 0.3, DENSE, next);     // x = 0 wall
    face(c, [0.3, 0, 0], X, Y, 0.3, 0.3, DENSE, next);   // floor starting 30 cm away
    expect(capability(c).readiness.capability.edge).toBe(false);
  });

  it('is area-invariant: same density over 4x area stays ready, same count over 4x area does not', () => {
    expect(capability(boxCorner([DENSE, DENSE, DENSE], 0.3, 21, 2)).readiness.capability.edge).toBe(true);
    expect(capability(boxCorner([DENSE, DENSE, DENSE], 0.6, 21, 2)).readiness.capability.edge).toBe(true);
    // Same point count over 4x area = double spacing = 1/4 density (7 cm = 204 /m^2, below the 216 target).
    expect(capability(boxCorner([0.035, 0.035, 0.035], 0.3, 21, 2)).readiness.capability.edge).toBe(true);
    expect(capability(boxCorner([0.07, 0.07, 0.07], 0.6, 21, 2)).readiness.capability.edge).toBe(false);
  });

  it('progress never decreases as the faces get denser, and reaches 1', () => {
    let previous = -1;
    for (const spacing of [0.09, 0.08, 0.07, 0.06, 0.05, 0.04, 0.03]) {
      const progress = capability(boxCorner([spacing, spacing, spacing], 0.3, 33)).readiness.progress;
      expect(progress, `spacing ${spacing}`).toBeGreaterThanOrEqual(previous - 1e-9);
      previous = progress;
    }
    expect(previous).toBe(1);
  });
});

describe('per-face density gate (component level, spec numbers)', () => {
  const voxel = 0.04;
  const target = structuralFaceDensityTarget(voxel);
  function component(id: number, pointCount: number, area: number, normal: V3): SurfaceComponent {
    const side = Math.sqrt(area);
    return {
      id, pointCount, occupiedCellCount: 50, center: [0, 0, 0], majorExtentMeters: side, minorExtentMeters: side,
      thicknessMeters: 0.005, occupancyRatio: 0.8, meshReadiness: 1, meshEligible: true, normal, estimatedAreaSquareMeters: area,
    };
  }
  // Two faces touching at the origin: a few points of each within 2 cm.
  const positions = [0, 0.01, 0.01, 0, 0.02, 0.03, 0.01, 0, 0.01, 0.02, 0, 0.03];
  const componentIdByPoint = [0, 0, 1, 1];
  function edge(a: SurfaceComponent, b: SurfaceComponent) {
    return computeStructuralFaceCapability({ components: [a, b], componentIdByPoint: Int32Array.from(componentIdByPoint) }, positions, voxel);
  }

  it('uses one point per (1.7 voxel)^2 as the face target', () => {
    expect(target).toBeCloseTo(1 / (0.068 * 0.068), 6);
  });

  it('a face 30 % above target makes an edge; a face 15 % below target does not, and is named', () => {
    const good = component(0, Math.ceil(target * 0.25 * 1.3), 0.25, [1, 0, 0]);
    expect(edge(good, component(1, Math.ceil(target * 0.25 * 1.3), 0.25, [0, 1, 0])).edgeProgress).toBe(1);
    const thin = edge(good, component(1, Math.floor(target * 0.25 * 0.85), 0.25, [0, 1, 0]));
    expect(thin.edgeProgress).toBeLessThan(1);
    expect(thin.faceProgress.find((f) => f.id === 1)!.limitingMetric).toBe('surface-density');
  });

  it('judges density per area: the same count over 4x the area fails, 4x the count over 4x the area passes', () => {
    const good = component(0, Math.ceil(target * 1.3), 1.0, [1, 0, 0]);
    const count = Math.ceil(target * 0.25 * 1.3);
    expect(edge(good, component(1, count, 0.25, [0, 1, 0])).edgeProgress).toBe(1);
    expect(edge(good, component(1, count, 1.0, [0, 1, 0])).edgeProgress).toBeLessThan(1);
    expect(edge(good, component(1, count * 4, 1.0, [0, 1, 0])).edgeProgress).toBe(1);
  });
});

describe('structural sufficiency end to end (harness scan -> production pipeline)', () => {
  function run(scene: SyntheticScene, poses: readonly CameraPose[]) {
    const accumulator = new DepthPointCloudAccumulator(0.04);
    const pipeline = new StructuralGuidePipeline();
    let last: ReturnType<StructuralGuidePipeline['analyze']> | null = null;
    poses.forEach((pose, index) => {
      const frame = renderDepth(scene, pose, index + 1, ARCORE_NOISE);
      accumulator.sampleDepthFrame(frame.depth, frame.view, { keyframeId: index + 1, evidenceKeyframe: true, frameWeight: 1, cameraProjectPosition: pose.eye });
      const g = accumulator.structuralGuideSnapshot();
      last = pipeline.analyze({ type: 'analyze', requestId: index, epoch: 0, positions: g.positions, normals: g.normals, confidence: g.confidence, keyframeCount: g.keyframeCount, voxelSizeMeters: g.voxelSizeMeters, viewpoint: { position: pose.eye, keyframeId: index + 1 } });
    });
    return last!;
  }

  it('a far, short scan whose faces are too small is NOT reported ready and yields no edges', () => {
    const scene = boxCornerScene();
    const { result, diagnostics } = run(scene, orbitPoses(scene.focus, 1.8, 35, 10, 80, 6));
    expect(diagnostics.structuralGeometryDeferred).toBe(true);
    expect(diagnostics.structuralSurfaceReadiness).toBeLessThan(1);
    expect(result.edges).toHaveLength(0);
  });

  it('a dense scan of the same corner is ready and the production pipeline finds the corner', () => {
    const scene = boxCornerScene();
    const { result, diagnostics } = run(scene, orbitPoses(scene.focus, 1.0, 35, 10, 80, 16));
    expect(diagnostics.structuralGeometryDeferred).toBe(false);
    const truth: Vec3 = scene.corners[0];
    const best = Math.min(Infinity, ...result.corners.map((c) => Math.hypot(c.position[0] - truth[0], c.position[1] - truth[1], c.position[2] - truth[2])));
    expect(best).toBeLessThanOrEqual(0.02);
  });

  it('negative control: a flat floor never becomes edge-ready however many poses, and the bar says FACE+', () => {
    const scene = tiltedPlaneScene(0, 0, 1.2);
    const accumulator = new DepthPointCloudAccumulator(0.04);
    const poses = orbitPoses(scene.focus, 1.2, 60, 0, 330, 16);
    poses.forEach((pose, index) => {
      const frame = renderDepth(scene, pose, index + 1, ARCORE_NOISE);
      accumulator.sampleDepthFrame(frame.depth, frame.view, { keyframeId: index + 1, evidenceKeyframe: true, frameWeight: 1, cameraProjectPosition: pose.eye });
    });
    const prepared = prepareStructuralGuideReadiness(accumulator.structuralGuideSnapshot());
    expect(prepared.structuralReadiness.capability.surface).toBe(true);
    expect(prepared.structuralReadiness.ready).toBe(false);
    expect(prepared.structuralReadiness.limitingMetric).toBe('faces');
    const readiness = computeScanCapabilityReadiness({
      stablePointCount: 500, keyframeCount: 16, visibleCoverageCells: 20, readyCoverageCells: 20,
      surfaceAnalysis: prepared.surfaceReadiness,
      viewpoints: poses.map((pose, index) => ({ position: pose.eye, keyframeId: index + 1 })),
    });
    const label = compactScanReadinessLabel(readiness, { visibleCorners: 0, certifiedCorners: 0, certifiedEdges: 0, rgbCorroboration: false });
    expect(label).not.toContain('S100');
    expect(label).toContain('FACE+');
  });
});

describe('coverage-cell readiness counts every face in the cell', () => {
  it('a corner cell is not ready while one of its faces is seen from a single viewpoint; single-face cells are', () => {
    // Corner in the middle of a 30 cm coverage cell so the three faces share it.
    const scene = boxCornerScene([0.15, 0.15, 0.15], 0.4);
    const accumulator = new DepthPointCloudAccumulator(0.04);
    const cornerCell: Vec3 = [0.1, 0.1, 0.1];
    const sideOnlyCells: Vec3[] = [[0.15, -0.15, -0.15], [-0.15, 0.15, -0.15]];
    // Near-horizontal orbit: the top face is at > 80 deg incidence and never measured.
    scanScene(accumulator, scene, orbitPoses(scene.focus, 1.0, 3, 5, 85, 8), { noise: ARCORE_NOISE });
    for (const cell of sideOnlyCells) expect(accumulator.coverageCellReadyAt(cell)).toBe(true);
    // One look from above: the top face now exists in the cell, but single-view.
    scanScene(accumulator, scene, orbitPoses(scene.focus, 1.0, 50, 45, 45, 1), { noise: ARCORE_NOISE, firstKeyframeId: 9 });
    expect(accumulator.coverageCellReadyAt(cornerCell)).toBe(false);
    for (const cell of sideOnlyCells) expect(accumulator.coverageCellReadyAt(cell)).toBe(true);
    // Independent views of the top face make the corner cell ready.
    scanScene(accumulator, scene, orbitPoses(scene.focus, 1.0, 55, 15, 75, 3), { noise: ARCORE_NOISE, firstKeyframeId: 10 });
    expect(accumulator.coverageCellReadyAt(cornerCell)).toBe(true);
  });
});
