import { describe, expect, it } from 'vitest';

import { computeStructuralFaceCapability } from '../../src/xr/scanCapabilityReadiness';
import { detectStructuralGuideGeometry, type StructuralGuideInput } from '../../src/xr/structuralGuide';
import type { SurfaceComponent } from '../../src/xr/surfaceComponents';

/*
 * MAX-256: an edge is decided by the INTERIOR dihedral measured from the
 * intersection line toward each face, not by |n_a . n_b| (which makes a 30 deg
 * wedge look like a 150 deg fold). Parallel faces (slabs) never form an edge.
 */

type V3 = [number, number, number];
const DEG = Math.PI / 180;

function rng(seed: number): () => number {
  let state = seed >>> 0;
  return () => { state = (Math.imul(state, 1664525) + 1013904223) >>> 0; return (state + 0.5) / 4294967296 - 0.5; };
}

/** Two half-planes meeting along the X axis with the given interior dihedral, outward normals, 4 mm noise. */
function wedge(dihedralDeg: number, seed = 1): StructuralGuideInput {
  const h = (dihedralDeg / 2) * DEG;
  const next = rng(seed + dihedralDeg);
  const faces: Array<[V3, V3]> = [
    [[0, Math.sin(h), -Math.cos(h)], [0, Math.cos(h), Math.sin(h)]],
    [[0, -Math.sin(h), -Math.cos(h)], [0, -Math.cos(h), Math.sin(h)]],
  ];
  const positions: number[] = []; const normals: number[] = []; const confidence: number[] = [];
  for (const [dir, n] of faces) for (let i = 0; i < 10; i += 1) for (let j = 0; j < 7; j += 1) {
    const x = -0.15 + (i + 0.5) * 0.03; const s = (j + 0.5) * 0.03; const e = 0.004 * next();
    positions.push(x, dir[1] * s + n[1] * e, dir[2] * s + n[2] * e); normals.push(...n); confidence.push(0.9);
  }
  return { positions: new Float32Array(positions), normals: new Float32Array(normals), confidence: new Float32Array(confidence), keyframeCount: 8, voxelSizeMeters: 0.04 };
}

function edgeOnXAxis(input: StructuralGuideInput): boolean {
  return detectStructuralGuideGeometry(input, false).edges.some((edge) =>
    Math.acos(Math.min(1, Math.abs(edge.direction[0]))) <= 3 * DEG && Math.hypot(edge.midpoint[1], edge.midpoint[2]) <= 0.01);
}

describe('acute edges by interior dihedral', () => {
  it('finds the edge (direction <= 3 deg, position <= 1 cm) for every dihedral from 30 to 145 deg', () => {
    const missed: number[] = [];
    for (let dihedral = 30; dihedral <= 145; dihedral += 5) if (!edgeOnXAxis(wedge(dihedral))) missed.push(dihedral);
    expect(missed).toEqual([]);
  });

  it('treats gentle folds (160-165 deg) as smooth, not as edges', () => {
    for (const dihedral of [160, 165]) expect(detectStructuralGuideGeometry(wedge(dihedral), false).edges).toHaveLength(0);
  });

  it('negative control: the two faces of a 2 cm slab never form an edge', () => {
    const next = rng(77);
    const positions: number[] = []; const normals: number[] = []; const confidence: number[] = [];
    for (let i = 0; i < 12; i += 1) for (let j = 0; j < 12; j += 1) {
      const x = (i + 0.5) * 0.03; const y = (j + 0.5) * 0.03;
      positions.push(x, y, 0.01 + 0.003 * next()); normals.push(0, 0, 1); confidence.push(0.9);
      positions.push(x, y, -0.01 + 0.003 * next()); normals.push(0, 0, -1); confidence.push(0.9);
    }
    const result = detectStructuralGuideGeometry({ positions: new Float32Array(positions), normals: new Float32Array(normals), confidence: new Float32Array(confidence), keyframeCount: 8, voxelSizeMeters: 0.04 }, false);
    expect(result.edges).toHaveLength(0);
  });

  it('negative control: one plane whose surfels carry both normal signs forms no edge', () => {
    const next = rng(78);
    const positions: number[] = []; const normals: number[] = []; const confidence: number[] = [];
    for (let i = 0; i < 12; i += 1) for (let j = 0; j < 12; j += 1) {
      positions.push((i + 0.5) * 0.03, (j + 0.5) * 0.03, 0.003 * next());
      normals.push(0, 0, (i + j) % 2 === 0 ? 1 : -1); confidence.push(0.9);
    }
    const result = detectStructuralGuideGeometry({ positions: new Float32Array(positions), normals: new Float32Array(normals), confidence: new Float32Array(confidence), keyframeCount: 8, voxelSizeMeters: 0.04 }, false);
    expect(result.edges).toHaveLength(0);
  });
});

describe('readiness uses the same dihedral rule', () => {
  function face(id: number, dihedralDeg: number, side: 1 | -1): SurfaceComponent {
    const h = (dihedralDeg / 2) * DEG;
    const dir: V3 = [0, side * Math.sin(h), -Math.cos(h)];
    return {
      id, pointCount: 100, occupiedCellCount: 40, center: [0, dir[1] * 0.15, dir[2] * 0.15], majorExtentMeters: 0.3, minorExtentMeters: 0.3,
      thicknessMeters: 0.005, occupancyRatio: 0.8, meshReadiness: 1, meshEligible: true,
      normal: [0, side * Math.cos(h), Math.sin(h)], estimatedAreaSquareMeters: 0.09,
    };
  }
  // A few points of each face within 2 cm of the shared edge, so the faces touch.
  const positions = [0, 0.005, -0.01, 0.05, 0.005, -0.012, 0, -0.005, -0.01, 0.05, -0.005, -0.012];
  const componentIdByPoint = Int32Array.from([0, 0, 1, 1]);

  it('a 30 deg wedge is edge-capable, a 160 deg fold is not', () => {
    const edge = (dihedral: number): number => computeStructuralFaceCapability(
      { components: [face(0, dihedral, 1), face(1, dihedral, -1)], componentIdByPoint }, positions, 0.04).edgeProgress;
    expect(edge(30)).toBe(1);
    expect(edge(90)).toBe(1);
    expect(edge(160)).toBe(0);
  });
});
