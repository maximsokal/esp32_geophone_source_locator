import { describe, expect, it } from 'vitest';

import { DepthPointCloudAccumulator } from '../../src/xr/depthPointCloudV2';
import { detectStructuralGuideGeometry, type StructuralGuideInput } from '../../src/xr/structuralGuide';
import { prepareStructuralGuideReadiness } from '../../src/xr/structuralGuideReadiness';
import { clusterStructuralSurfaceFamilies } from '../../src/xr/structuralSurfaceClustering';
import {
  ARCORE_NOISE, orbitPoses, rectangle, ridgeScene, scanScene, type CameraPose, type SyntheticScene,
} from './support/syntheticDepthScene';

/*
 * MAX-260: normals keep their observation side end to end (accumulator
 * association, structural clustering, patches, face gates, components). The
 * two faces of a knife edge have nearly opposite normals; sign-less gates
 * merged them, so wedges below ~30 deg were lost. Decision: a surface seen
 * from both sides is two oriented surfaces, which never form an edge.
 */

const DEG = Math.PI / 180;

function rng(seed: number): () => number {
  let state = seed >>> 0;
  return () => { state = (Math.imul(state, 1664525) + 1013904223) >>> 0; return (state + 0.5) / 4294967296 - 0.5; };
}

/** Wedge along X with the given interior dihedral, outward normals, 4 mm noise. */
function wedgeCloud(dihedralDeg: number) {
  const h = (dihedralDeg / 2) * DEG;
  const next = rng(5 + dihedralDeg);
  const positions: number[] = []; const normals: number[] = []; const confidence: number[] = [];
  const faces: Array<[number[], number[]]> = [
    [[0, Math.sin(h), -Math.cos(h)], [0, Math.cos(h), Math.sin(h)]],
    [[0, -Math.sin(h), -Math.cos(h)], [0, -Math.cos(h), Math.sin(h)]],
  ];
  for (const [dir, n] of faces) for (let i = 0; i < 10; i += 1) for (let j = 0; j < 7; j += 1) {
    const x = -0.15 + (i + 0.5) * 0.03; const s = (j + 0.5) * 0.03; const e = 0.004 * next();
    positions.push(x, dir[1] * s + n[1] * e, dir[2] * s + n[2] * e); normals.push(n[0], n[1], n[2]); confidence.push(0.9);
  }
  return { positions: new Float32Array(positions), normals: new Float32Array(normals), confidence: new Float32Array(confidence), pointCount: confidence.length, keyframeCount: 8, voxelSizeMeters: 0.04 };
}

/** Structural surface exactly as the production pipeline prepares it. */
function productionSurface(raw: ReturnType<typeof wedgeCloud>): StructuralGuideInput {
  const s = prepareStructuralGuideReadiness(raw).structuralSurface;
  return { positions: s.positions, normals: s.normals, confidence: s.confidence, keyframeCount: 8, voxelSizeMeters: 0.04 };
}

function ridgeEdgeFound(input: StructuralGuideInput): boolean {
  return detectStructuralGuideGeometry(input, false).edges.some((edge) =>
    Math.acos(Math.min(1, Math.abs(edge.direction[0]))) <= 3 * DEG && Math.hypot(edge.midpoint[1], edge.midpoint[2]) <= 0.01);
}

describe('knife edges through the production preparation (signed normals)', () => {
  it('finds wedges of 20-40 deg within 3 deg / 1 cm (sign-less clustering merged their faces below 30 deg)', () => {
    const missed: number[] = [];
    for (const dihedral of [20, 25, 30, 35, 40]) if (!ridgeEdgeFound(productionSurface(wedgeCloud(dihedral)))) missed.push(dihedral);
    expect(missed).toEqual([]);
  });

  it('finds 20-35 deg wedges on real harness scans (sign-less surfel association merged the faces at the tip)', () => {
    const missed: number[] = [];
    for (const dihedral of [20, 25, 30, 35]) {
      const scene = ridgeScene(dihedral);
      const accumulator = new DepthPointCloudAccumulator(0.04);
      scanScene(accumulator, scene, orbitPoses(scene.focus, 0.8, 30, -60, 240, 20), { noise: ARCORE_NOISE });
      const s = prepareStructuralGuideReadiness(accumulator.structuralGuideSnapshot()).structuralSurface;
      if (!ridgeEdgeFound({ positions: s.positions, normals: s.normals, confidence: s.confidence, keyframeCount: 8, voxelSizeMeters: 0.04 })) missed.push(dihedral);
    }
    expect(missed).toEqual([]);
  });
});

describe('a surface seen from both sides', () => {
  it('stays two oriented surfaces in the accumulator: no blended normals, and never an edge', () => {
    // A zero-thickness sheet scanned from above and from below.
    const scene: SyntheticScene = { name: 'sheet', polygons: [rectangle([0, 0, 0], [0.3, 0, 0], [0, 0.3, 0], 'sheet')], creases: [], corners: [], focus: [0, 0, 0] };
    const above: CameraPose[] = orbitPoses([0, 0, 0], 1.0, 60, 0, 240, 6);
    const below: CameraPose[] = orbitPoses([0, 0, 0], 1.0, -60, 0, 240, 6);
    const accumulator = new DepthPointCloudAccumulator(0.04);
    scanScene(accumulator, scene, [...above, ...below], { noise: ARCORE_NOISE });
    const snapshot = accumulator.snapshot(12, 12);
    let up = 0; let down = 0; let blended = 0;
    for (let i = 0; i < snapshot.pointCount; i += 1) {
      const nz = snapshot.normals[i * 3 + 2];
      if (nz >= 0.95) up += 1; else if (nz <= -0.95) down += 1; else blended += 1;
    }
    expect(up).toBeGreaterThan(30);
    expect(down).toBeGreaterThan(30);
    // Tolerance: a handful of rim samples at the sheet border.
    expect(blended).toBeLessThanOrEqual(Math.ceil(snapshot.pointCount * 0.05));
    const g = accumulator.structuralGuideSnapshot();
    expect(detectStructuralGuideGeometry({ positions: g.positions, normals: g.normals, confidence: g.confidence, keyframeCount: 12, voxelSizeMeters: 0.04 }, false).edges).toHaveLength(0);
  });

  it('keeps opposite-facing surfels as separate structural families', () => {
    const result = clusterStructuralSurfaceFamilies({
      positions: Float32Array.from([0.010, 0.010, 0.010, 0.012, 0.011, 0.010]),
      normals: Float32Array.from([1, 0, 0, -1, 0, 0]),
      confidence: Float32Array.from([0.9, 0.9]),
      pointCount: 2, keyframeCount: 8, voxelSizeMeters: 0.04,
    });
    expect(result.pointCount).toBe(2);
    expect(Math.sign(result.normals[0]) * Math.sign(result.normals[3])).toBe(-1);
  });
});
