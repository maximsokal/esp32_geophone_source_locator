import { describe, expect, it } from 'vitest';

import { DepthPointCloudAccumulator } from '../../src/xr/depthPointCloudV2';
import { detectStructuralGuideGeometry, type StructuralGuideInput } from '../../src/xr/structuralGuide';
import {
  ARCORE_NOISE, boxCornerScene, insideCornerScene, orbitPoses, ridgeScene, scanScene,
  type CameraPose, type SyntheticScene, type Vec3,
} from './support/syntheticDepthScene';

/*
 * MAX-258: edges and corners must be found at the density the accumulator
 * really delivers (~400 structural points/m^2), without bridging real gaps.
 */

const DEG = Math.PI / 180;

function scanInput(scene: SyntheticScene, poses: readonly CameraPose[]): StructuralGuideInput {
  const accumulator = new DepthPointCloudAccumulator(0.04);
  scanScene(accumulator, scene, poses, { noise: ARCORE_NOISE });
  const raw = accumulator.structuralGuideSnapshot();
  return { positions: raw.positions, normals: raw.normals, confidence: raw.confidence, keyframeCount: raw.keyframeCount, voxelSizeMeters: raw.voxelSizeMeters };
}

function nearestCorner(corners: ReadonlyArray<{ position: readonly [number, number, number] }>, truth: Vec3): number {
  return Math.min(Infinity, ...corners.map((c) => Math.hypot(c.position[0] - truth[0], c.position[1] - truth[1], c.position[2] - truth[2])));
}

function rng(seed: number): () => number {
  let state = seed >>> 0;
  return () => { state = (Math.imul(state, 1664525) + 1013904223) >>> 0; return (state + 0.5) / 4294967296 - 0.5; };
}

interface Cloud { positions: number[]; normals: number[]; confidence: number[] }
const cloud = (): Cloud => ({ positions: [], normals: [], confidence: [] });
function add(c: Cloud, p: Vec3, n: Vec3): void { c.positions.push(...p); c.normals.push(...n); c.confidence.push(0.9); }
function input(c: Cloud): StructuralGuideInput {
  return { positions: new Float32Array(c.positions), normals: new Float32Array(c.normals), confidence: new Float32Array(c.confidence), keyframeCount: 8, voxelSizeMeters: 0.04 };
}

describe('structural detection on real (harness) scans', () => {
  it('box corner scanned from 16 poses at 1 m: >= 3 edges and the corner within 2 cm', () => {
    const scene = boxCornerScene();
    const result = detectStructuralGuideGeometry(scanInput(scene, orbitPoses(scene.focus, 1.0, 35, 10, 80, 16)));
    expect(result.edges.length).toBeGreaterThanOrEqual(3);
    expect(nearestCorner(result.corners, scene.corners[0])).toBeLessThanOrEqual(0.02);
  });

  it('concave inside corner: the corner within 2 cm', () => {
    const scene = insideCornerScene();
    const result = detectStructuralGuideGeometry(scanInput(scene, orbitPoses(scene.focus, 1.2, 40, 10, 80, 16)));
    expect(nearestCorner(result.corners, scene.corners[0])).toBeLessThanOrEqual(0.02);
  });

  it('90 deg ridge: an edge along the ridge with direction error <= 3 deg', () => {
    const scene = ridgeScene(90);
    const result = detectStructuralGuideGeometry(scanInput(scene, orbitPoses(scene.focus, 1.2, 55, 0, 315, 12)));
    const along = result.edges.map((edge) => Math.acos(Math.min(1, Math.abs(edge.direction[0]))) / DEG);
    expect(along.length).toBeGreaterThanOrEqual(1);
    expect(Math.min(...along)).toBeLessThanOrEqual(3);
  });
});

describe('structural detection across sampling densities', () => {
  it('finds the box corner within 1.5 cm at 3, 4 and 5 cm spacing with 4 mm noise, wherever the grid falls', () => {
    const failures: string[] = [];
    for (const offset of [0, 0.023, 0.05]) for (const spacing of [0.03, 0.04, 0.05]) {
      const next = rng(Math.round(offset * 1000) * 31 + Math.round(spacing * 1000));
      const c = cloud();
      const n = Math.round(0.4 / spacing);
      for (let i = 0; i < n; i += 1) for (let j = 0; j < n; j += 1) {
        const a = (i + 0.5) * spacing; const b = (j + 0.5) * spacing;
        add(c, [offset + 0.008 * next(), offset - a, offset - b], [1, 0, 0]);
        add(c, [offset - a, offset + 0.008 * next(), offset - b], [0, 1, 0]);
        add(c, [offset - a, offset - b, offset + 0.008 * next()], [0, 0, 1]);
      }
      const result = detectStructuralGuideGeometry(input(c));
      const error = nearestCorner(result.corners, [offset, offset, offset]);
      if (!(error <= 0.015)) failures.push(`offset ${offset} spacing ${spacing}: ${result.edges.length} edges, corner error ${error}`);
    }
    expect(failures).toEqual([]);
  });
});

describe('structural detection negative controls', () => {
  it('never bridges a 15 cm gap in one face along the would-be edge', () => {
    const c = cloud();
    const next = rng(99);
    for (let y = -0.2; y <= 0.2 + 1e-9; y += 0.03) for (let z = 0; z <= 0.4 + 1e-9; z += 0.03) add(c, [0.004 * next(), y, z], [1, 0, 0]);
    for (let x = -0.2; x <= 0.2 + 1e-9; x += 0.03) for (let z = 0; z <= 0.4 + 1e-9; z += 0.03) {
      if (z > 0.12 && z < 0.27) continue; // the gap in face B
      add(c, [x, 0.004 * next(), z], [0, 1, 0]);
    }
    const result = detectStructuralGuideGeometry(input(c));
    // Positive control: the supported pieces still produce edges.
    expect(result.edges.length).toBeGreaterThan(0);
    for (const edge of result.edges) {
      const zMin = Math.min(edge.start[2], edge.end[2]); const zMax = Math.max(edge.start[2], edge.end[2]);
      expect(zMin < 0.14 && zMax > 0.25, `edge ${zMin.toFixed(3)}..${zMax.toFixed(3)} spans the gap`).toBe(false);
    }
  });

  it('two parallel walls facing each other 10 cm apart produce no edge', () => {
    const c = cloud();
    const next = rng(5);
    for (let y = -0.2; y <= 0.2 + 1e-9; y += 0.03) for (let z = 0; z <= 0.4 + 1e-9; z += 0.03) {
      add(c, [0.004 * next(), y, z], [1, 0, 0]);
      add(c, [0.1 + 0.004 * next(), y, z], [-1, 0, 0]);
    }
    const result = detectStructuralGuideGeometry(input(c));
    expect(result.edges).toHaveLength(0);
    expect(result.corners).toHaveLength(0);
  });
});
