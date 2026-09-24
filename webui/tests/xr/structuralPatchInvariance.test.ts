import { describe, expect, it } from 'vitest';

import { detectStructuralGuideGeometry, structuralGuideMathForTests } from '../../src/xr/structuralGuide';

/*
 * MAX-255: plane patches and the corners built from them must not depend on
 * the object's orientation or on where it sits relative to the 10 cm patch
 * grid. All clouds are analytic with seeded noise.
 */

type V3 = [number, number, number];
const PATCH_CELL = 0.10;
const DEG = Math.PI / 180;

function rng(seed: number): () => number {
  let state = seed >>> 0;
  return () => { state = (Math.imul(state, 1664525) + 1013904223) >>> 0; return (state + 0.5) / 4294967296; };
}

function rotateVector(q: readonly [number, number, number, number], v: V3): V3 {
  const [x, y, z, w] = q;
  const tx = 2 * (y * v[2] - z * v[1]); const ty = 2 * (z * v[0] - x * v[2]); const tz = 2 * (x * v[1] - y * v[0]);
  return [v[0] + w * tx + (y * tz - z * ty), v[1] + w * ty + (z * tx - x * tz), v[2] + w * tz + (x * ty - y * tx)];
}

/** Uniform random rotation (Shoemake). */
function randomRotation(next: () => number): [number, number, number, number] {
  const u1 = next(), u2 = next() * 2 * Math.PI, u3 = next() * 2 * Math.PI;
  const a = Math.sqrt(1 - u1), b = Math.sqrt(u1);
  return [a * Math.sin(u2), a * Math.cos(u2), b * Math.sin(u3), b * Math.cos(u3)];
}

interface Cloud { positions: number[]; normals: number[]; confidence: number[] }

function jitterNormal(normal: V3, degrees: number, next: () => number): V3 {
  const helper: V3 = Math.abs(normal[0]) < 0.9 ? [1, 0, 0] : [0, 1, 0];
  const t1: V3 = [normal[1] * helper[2] - normal[2] * helper[1], normal[2] * helper[0] - normal[0] * helper[2], normal[0] * helper[1] - normal[1] * helper[0]];
  const l1 = Math.hypot(...t1); t1[0] /= l1; t1[1] /= l1; t1[2] /= l1;
  const t2: V3 = [normal[1] * t1[2] - normal[2] * t1[1], normal[2] * t1[0] - normal[0] * t1[2], normal[0] * t1[1] - normal[1] * t1[0]];
  const a = (next() - 0.5) * 2 * degrees * DEG; const b = (next() - 0.5) * 2 * degrees * DEG;
  const m: V3 = [normal[0] + t1[0] * a + t2[0] * b, normal[1] + t1[1] * a + t2[1] * b, normal[2] + t1[2] * a + t2[2] * b];
  const l = Math.hypot(...m);
  return [m[0] / l, m[1] / l, m[2] / l];
}

/** Square grid on a plane through `origin` spanned by unit u, v (normal u x v). */
function planeCloud(cloud: Cloud, origin: V3, u: V3, v: V3, size: number, spacing: number, normalJitterDeg: number, positionJitter: number, next: () => number): void {
  const normal: V3 = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]];
  const n = Math.round(size / spacing);
  for (let i = 0; i < n; i += 1) for (let j = 0; j < n; j += 1) {
    const s = (i + 0.5) * spacing; const t = (j + 0.5) * spacing; const e = (next() - 0.5) * 2 * positionJitter;
    cloud.positions.push(origin[0] + u[0] * s + v[0] * t + normal[0] * e, origin[1] + u[1] * s + v[1] * t + normal[1] * e, origin[2] + u[2] * s + v[2] * t + normal[2] * e);
    cloud.normals.push(...jitterNormal(normal, normalJitterDeg, next));
    cloud.confidence.push(0.9);
  }
}

function toInput(cloud: Cloud) {
  return { positions: new Float32Array(cloud.positions), normals: new Float32Array(cloud.normals), confidence: new Float32Array(cloud.confidence), keyframeCount: 8, voxelSizeMeters: 0.04 };
}

describe('plane patches are orientation independent', () => {
  it('gives exactly one patch per well-populated cell for a single plane at every tilt (1 deg steps) and yaw', () => {
    const failures: string[] = [];
    for (const yaw of [0, 17, 45]) for (let tilt = 0; tilt <= 80; tilt += 1) {
      const next = rng(tilt * 1000 + yaw);
      const t = tilt * DEG; const y = yaw * DEG;
      const rot = (w: V3): V3 => {
        const x1 = w[0] * Math.cos(t) + w[2] * Math.sin(t); const z1 = -w[0] * Math.sin(t) + w[2] * Math.cos(t);
        return [x1 * Math.cos(y) - w[1] * Math.sin(y), x1 * Math.sin(y) + w[1] * Math.cos(y), z1];
      };
      const cloud: Cloud = { positions: [], normals: [], confidence: [] };
      planeCloud(cloud, [0.013, 0.021, 0.007], rot([1, 0, 0]), rot([0, 1, 0]), 0.4, 0.03, 8, 0, next);
      const patches = structuralGuideMathForTests.buildPlanePatches(toInput(cloud));
      const perCell = new Map<string, number>();
      for (const patch of patches) perCell.set(patch.cell, (perCell.get(patch.cell) ?? 0) + 1);
      const pointsPerCell = new Map<string, number>();
      for (let i = 0; i < cloud.positions.length; i += 3) {
        const key = `${Math.floor(cloud.positions[i] / PATCH_CELL)}:${Math.floor(cloud.positions[i + 1] / PATCH_CELL)}:${Math.floor(cloud.positions[i + 2] / PATCH_CELL)}`;
        pointsPerCell.set(key, (pointsPerCell.get(key) ?? 0) + 1);
      }
      for (const [cell, points] of pointsPerCell) {
        const count = perCell.get(cell) ?? 0;
        // Cells with >= 6 points have enough spread for a plane fit; they must yield exactly one patch.
        if (count > 1 || (points >= 6 && count !== 1)) failures.push(`tilt ${tilt} yaw ${yaw} cell ${cell}: ${points} points -> ${count} patches`);
      }
    }
    expect(failures.slice(0, 10)).toEqual([]);
  });
});

describe('corners are invariant to rigid placement', () => {
  function boxCorner(next: () => number): Cloud {
    const cloud: Cloud = { positions: [], normals: [], confidence: [] };
    // Faces x = 0, y = 0, z = 0 of the box [-0.3, 0]^3, corner at the origin.
    planeCloud(cloud, [0, -0.3, -0.3], [0, 1, 0], [0, 0, 1], 0.3, 0.03, 5, 0.004, next);
    planeCloud(cloud, [-0.3, 0, -0.3], [0, 0, 1], [1, 0, 0], 0.3, 0.03, 5, 0.004, next);
    planeCloud(cloud, [-0.3, -0.3, 0], [1, 0, 0], [0, 1, 0], 0.3, 0.03, 5, 0.004, next);
    return cloud;
  }

  it('finds the box corner within 1.5 cm in >= 98 % of 200 seeded rigid placements', () => {
    // 4 mm position noise and 3 cm spacing; 1.5 cm = 3/8 of a 4 cm voxel,
    // well above the noise-limited three-plane intersection error (~few mm).
    const placement = rng(0x51ed);
    const misses: string[] = [];
    for (let trial = 0; trial < 200; trial += 1) {
      const q = randomRotation(placement);
      const offset: V3 = [placement() * PATCH_CELL, placement() * PATCH_CELL, placement() * PATCH_CELL];
      const cloud = boxCorner(rng(1000 + trial));
      const positions: number[] = []; const normals: number[] = [];
      for (let i = 0; i < cloud.positions.length; i += 3) {
        const p = rotateVector(q, [cloud.positions[i], cloud.positions[i + 1], cloud.positions[i + 2]]);
        positions.push(p[0] + offset[0], p[1] + offset[1], p[2] + offset[2]);
        normals.push(...rotateVector(q, [cloud.normals[i], cloud.normals[i + 1], cloud.normals[i + 2]]));
      }
      const result = detectStructuralGuideGeometry(toInput({ positions, normals, confidence: cloud.confidence }));
      const best = Math.min(Infinity, ...result.corners.map((c) => Math.hypot(c.position[0] - offset[0], c.position[1] - offset[1], c.position[2] - offset[2])));
      if (!(best <= 0.015)) misses.push(`trial ${trial}: best ${best.toFixed(3)} m, corners ${result.corners.length}`);
    }
    // 200 placements resolve a 2 % miss rate; a corner search that only accepts
    // the lowest-index patch as hub misses ~5 % here and must fail.
    expect(misses.length, misses.join('\n')).toBeLessThanOrEqual(4);
  });

  it('negative control: a dense cylinder (r = 0.5 m) yields no corners and no edges at any orientation', () => {
    const placement = rng(0xc11);
    for (let trial = 0; trial < 10; trial += 1) {
      const q = randomRotation(placement);
      const next = rng(trial + 77);
      const positions: number[] = []; const normals: number[] = []; const confidence: number[] = [];
      for (let a = 0; a < 70; a += 1) for (let h = 0; h < 14; h += 1) {
        const angle = (a / 70) * 1.3; // 75 deg arc, arc spacing ~9 mm... sampled at 3 cm along height
        const radial: V3 = [Math.cos(angle), Math.sin(angle), 0];
        const noise = (next() - 0.5) * 0.008;
        const p = rotateVector(q, [radial[0] * (0.5 + noise), radial[1] * (0.5 + noise), h * 0.03]);
        positions.push(...p);
        normals.push(...rotateVector(q, jitterNormal(radial, 5, next)));
        confidence.push(0.9);
      }
      const result = detectStructuralGuideGeometry(toInput({ positions, normals, confidence }));
      expect(result.corners.length, `trial ${trial}`).toBe(0);
      expect(result.edges.length, `trial ${trial}`).toBe(0);
    }
  });
});
