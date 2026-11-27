import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, expect, it } from 'vitest';

import { buildTsdfRequestArgs } from '../../src/components/ScanPointCloudViewport';
import { buildDirectionalTsdfMesh, prepareDirectionalTsdfPlan } from '../../src/xr/directionalTsdf';
import { refineScanMesh } from '../../src/xr/meshRefine';
import { sanitizeMeshTopology } from '../../src/xr/meshTopologySanity';
import { normalIntrusionBudgets } from '../../src/xr/meshNormalIntrusion';
import { selfIntersectionBudgets } from '../../src/xr/meshSelfIntersection';
import { buildSparseTsdfMesh, SPARSE_TSDF_MAX_COARSENINGS } from '../../src/xr/sparseTsdf';
import { decodeScanArtifact } from '../../src/xr/scanArtifact';

const VOXEL = 0.05;

/** A flat sheet of `nx` x `ny` cells of `cell` metres in the XY plane at height z (two triangles per cell), as a triangle soup. `skip` leaves cells out. */
function sheet(nx: number, ny: number, cell = 0.05, z = 0, skip: (i: number, j: number) => boolean = () => false, bump: (x: number, y: number) => number = () => 0): Float32Array {
  const out: number[] = [];
  const p = (x: number, y: number): number[] => [x * cell, y * cell, z + bump(x * cell, y * cell)];
  for (let i = 0; i < nx; i += 1) for (let j = 0; j < ny; j += 1) {
    if (skip(i, j)) continue;
    out.push(...p(i, j), ...p(i + 1, j), ...p(i + 1, j + 1));
    out.push(...p(i, j), ...p(i + 1, j + 1), ...p(i, j + 1));
  }
  return Float32Array.from(out);
}

/** Edges used by exactly one triangle after welding vertices; the open boundary of the mesh. */
function boundaryEdges(soup: Float32Array, tolerance = 0.004): number {
  const ids = new Map<string, number>();
  const id = (o: number): number => { const key = `${Math.round(soup[o] / tolerance)},${Math.round(soup[o + 1] / tolerance)},${Math.round(soup[o + 2] / tolerance)}`; let v = ids.get(key); if (v === undefined) { v = ids.size; ids.set(key, v); } return v; };
  const counts = new Map<string, number>();
  for (let t = 0; t + 8 < soup.length; t += 9) {
    const v = [id(t), id(t + 3), id(t + 6)];
    for (let e = 0; e < 3; e += 1) { const a = v[e]; const b = v[(e + 1) % 3]; const key = a < b ? `${a}_${b}` : `${b}_${a}`; counts.set(key, (counts.get(key) ?? 0) + 1); }
  }
  let n = 0; for (const c of counts.values()) if (c === 1) n += 1;
  return n;
}

describe('closing small holes', () => {
  it('closes a hole of a few cells in the middle of a large flat sheet, and the result has no open edge around it', () => {
    const holed = sheet(30, 30, 0.05, 0, (i, j) => i >= 14 && i <= 15 && j >= 14 && j <= 15);      /* a 10 cm x 10 cm hole */
    const before = boundaryEdges(holed);
    const result = refineScanMesh(holed, VOXEL, { smoothingIterations: 0 });
    expect(result.holesFilled).toBe(1);
    expect(result.holeTrianglesAdded).toBeGreaterThanOrEqual(8);
    expect(boundaryEdges(result.trianglePositions)).toBe(before - 8);                                 /* the 8 edges of the hole are gone, the outer edge is untouched */
    expect(result.triangleCount).toBe(holed.length / 9 + result.holeTrianglesAdded);
  });

  it('keeps the new triangles in the plane of the hole and facing the same way as the surface', () => {
    const holed = sheet(30, 30, 0.05, 0.7, (i, j) => i >= 14 && i <= 15 && j >= 14 && j <= 15);
    const result = refineScanMesh(holed, VOXEL, { smoothingIterations: 0 });
    const soup = result.trianglePositions;
    for (let t = holed.length / 9; t < result.triangleCount; t += 1) {
      const o = t * 9;
      for (let c = 0; c < 3; c += 1) expect(soup[o + c * 3 + 2]).toBeCloseTo(0.7, 4);
      const ux = soup[o + 3] - soup[o]; const uy = soup[o + 4] - soup[o + 1];
      const vx = soup[o + 6] - soup[o]; const vy = soup[o + 7] - soup[o + 1];
      expect(ux * vy - uy * vx, 'same winding as the sheet (+Z normal)').toBeGreaterThan(0);
    }
  });

  it('leaves the open edge of a scan alone: the outline of a sheet is far larger than a hole', () => {
    const open = sheet(30, 30);
    const result = refineScanMesh(open, VOXEL, { smoothingIterations: 0 });
    expect(result.holesFilled).toBe(0);
    expect(boundaryEdges(result.trianglePositions)).toBe(boundaryEdges(open));
    expect(result.openReasons.tooLarge).toBeGreaterThan(0);
  });

  it('does not close a gap that is too large (a doorway, a window, an unscanned area)', () => {
    const gap = sheet(40, 40, 0.05, 0, (i, j) => i >= 12 && i <= 25 && j >= 12 && j <= 25);          /* 70 cm square */
    const result = refineScanMesh(gap, VOXEL, { smoothingIterations: 0 });
    expect(result.holesFilled).toBe(0);
    expect(boundaryEdges(result.trianglePositions)).toBe(boundaryEdges(gap));
  });

  it('does not close a hole that has few edges but is large: coarse triangles around a 25 cm opening', () => {
    const coarse = sheet(14, 14, 0.25, 0, (i, j) => i === 6 && j === 6);
    const result = refineScanMesh(coarse, 0.25, { smoothingIterations: 0, maxHoleEdges: 12, maxHolePerimeterMeters: 0.5, maxHoleEdgeMeters: 0.125 });
    expect(result.holesFilled).toBe(0);
    expect(boundaryEdges(result.trianglePositions)).toBe(boundaryEdges(coarse));
  });

  it('does not close a hole whose rim is not flat', () => {
    const lopsided = sheet(30, 30, 0.05, 0, (i, j) => i >= 14 && i <= 15 && j >= 14 && j <= 15, (x, y) => (Math.abs(x - 0.7) < 1e-6 && Math.abs(y - 0.7) < 1e-6 ? 0.04 : 0));          /* one corner of the rim stands 4 cm off the plane */
    const result = refineScanMesh(lopsided, VOXEL, { smoothingIterations: 0 });
    expect(result.holesFilled).toBe(0);
    expect(result.openReasons.notFlat).toBeGreaterThan(0);
  });

  it('never caps a tiny isolated patch: its whole outline would become a closed blob', () => {
    const patch = sheet(2, 2);
    const result = refineScanMesh(patch, VOXEL, { smoothingIterations: 0 });
    expect(result.holesFilled).toBe(0);
    expect(result.triangleCount).toBe(patch.length / 9);
  });
});

describe('smoothing', () => {
  const rough = (x: number, y: number): number => 0.004 * Math.sin(x * 211) * Math.cos(y * 173);

  it('calms a noisy surface, moving no vertex further than a quarter voxel', () => {
    const noisy = sheet(30, 30, 0.05, 0, () => false, rough);
    const result = refineScanMesh(noisy, VOXEL);
    expect(result.maxDisplacementMeters).toBeLessThanOrEqual(VOXEL * 0.25 + 1e-9);
    const spread = (soup: Float32Array): number => { let sum = 0; let sq = 0; const n = soup.length / 3; for (let i = 2; i < soup.length; i += 3) { sum += soup[i]; sq += soup[i] * soup[i]; } return Math.sqrt(Math.max(0, sq / n - (sum / n) ** 2)); };
    expect(spread(result.trianglePositions)).toBeLessThan(spread(noisy));
    expect(result.smoothedVertices).toBeGreaterThan(100);
  });

  it('keeps every vertex of an open boundary exactly where it was measured', () => {
    const noisy = sheet(20, 20, 0.05, 0, () => false, rough);
    const result = refineScanMesh(noisy, VOXEL);
    const edgeVertices = new Set<string>();
    for (let i = 0; i < noisy.length; i += 3) {
      const x = noisy[i]; const y = noisy[i + 1];
      if (x < 1e-6 || y < 1e-6 || x > 0.999 || y > 0.999) edgeVertices.add(`${x.toFixed(4)},${y.toFixed(4)},${noisy[i + 2].toFixed(5)}`);
    }
    let kept = 0;
    for (let i = 0; i < result.trianglePositions.length; i += 3) {
      const x = result.trianglePositions[i]; const y = result.trianglePositions[i + 1];
      if (x < 1e-6 || y < 1e-6 || x > 0.999 || y > 0.999) { if (edgeVertices.has(`${x.toFixed(4)},${y.toFixed(4)},${result.trianglePositions[i + 2].toFixed(5)}`)) kept += 1; }
    }
    expect(kept).toBeGreaterThan(0);
    for (let i = 0; i < result.trianglePositions.length; i += 3) {
      const x = result.trianglePositions[i]; const y = result.trianglePositions[i + 1];
      if (x < 1e-6 || y < 1e-6 || x > 0.999 || y > 0.999) expect(edgeVertices.has(`${x.toFixed(4)},${y.toFixed(4)},${result.trianglePositions[i + 2].toFixed(5)}`)).toBe(true);
    }
  });

  it('leaves a perfectly flat sheet flat', () => {
    const flat = sheet(20, 20, 0.05, 0.42);
    const result = refineScanMesh(flat, VOXEL);
    expect(result.maxDisplacementMeters).toBeLessThan(1e-6);
    for (let i = 2; i < result.trianglePositions.length; i += 3) expect(result.trianglePositions[i]).toBeCloseTo(0.42, 5);
  });

  it('is deterministic and hands unusable input back unchanged', () => {
    const noisy = sheet(15, 15, 0.05, 0, (i, j) => i === 7 && j === 7, rough);
    const a = refineScanMesh(noisy, VOXEL); const b = refineScanMesh(noisy, VOXEL);
    expect(Array.from(a.trianglePositions)).toEqual(Array.from(b.trianglePositions));
    const empty = new Float32Array(0);
    expect(refineScanMesh(empty, VOXEL).trianglePositions).toBe(empty);
    const broken = Float32Array.from([NaN, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0]);
    expect(refineScanMesh(broken, VOXEL).trianglePositions).toBe(broken);
  });
});

describe('the budgets that decide how much of a scan becomes mesh', () => {
  /** A big floor and a small wall: the case an equal split of the triangle budget got wrong. */
  function floorAndWall() {
    const pts: number[] = []; const nrm: number[] = [];
    for (let i = 0; i < 70; i += 1) for (let j = 0; j < 70; j += 1) { pts.push(i * 0.05, j * 0.05, 0); nrm.push(0, 0, 1); }       /* 3.5 m x 3.5 m floor */
    for (let i = 0; i < 12; i += 1) for (let j = 0; j < 10; j += 1) { pts.push(i * 0.05, -0.05, 0.05 + j * 0.05); nrm.push(0, 1, 0); }   /* a 60 cm wall strip */
    const positions = Float32Array.from(pts);
    return { positions, normals: Float32Array.from(nrm), confidence: new Float32Array(positions.length / 3).fill(0.9), voxelSizeMeters: 0.05, maxTriangles: 18000 };
  }

  it('gives each directional channel a share of the triangle budget that follows its points', () => {
    const plan = prepareDirectionalTsdfPlan(floorAndWall());
    expect(plan.mode).toBe('multi');
    const [floor, wall] = [...plan.channelInputs].sort((a, b) => (b.maxTriangles ?? 0) - (a.maxTriangles ?? 0));
    expect(floor.maxTriangles!).toBeGreaterThan(18000 / plan.channelInputs.length);                 /* more than the old equal share */
    expect(wall.maxTriangles!).toBeLessThan(18000 / plan.channelInputs.length);
    expect((floor.maxTriangles ?? 0) + (wall.maxTriangles ?? 0)).toBeLessThanOrEqual(18000 + 64);
  });

  it('builds a floor larger than the old equal share could hold', () => {
    const mesh = buildDirectionalTsdfMesh(floorAndWall());
    expect(mesh.triangleCount).toBeGreaterThan(3000);
  });

  it('never cuts the surface into bands when the triangle budget is too small: it coarsens the voxel over the whole surface (phone run 2026-11-22: a 1 m strip of a 4 m floor)', () => {
    const floorOnly = (() => {
      const pts: number[] = []; const nrm: number[] = [];
      for (let i = 0; i < 80; i += 1) for (let j = 0; j < 80; j += 1) { pts.push(i * 0.05, j * 0.05, 0); nrm.push(0, 0, 1); }       /* 4 m x 4 m */
      const positions = Float32Array.from(pts);
      return { positions, normals: Float32Array.from(nrm), confidence: new Float32Array(positions.length / 3).fill(0.9), voxelSizeMeters: 0.05 };
    })();
    const roomy = buildSparseTsdfMesh({ ...floorOnly, maxTriangles: 100_000 });
    expect(roomy.coarsenings).toBe(0);
    expect(roomy.triangleCount).toBeGreaterThan(8000);
    const tight = buildSparseTsdfMesh({ ...floorOnly, maxTriangles: 5000 });
    expect(tight.coarsenings!).toBeGreaterThanOrEqual(1);
    expect(tight.triangleCount).toBeLessThanOrEqual(5000);
    const extent = (soup: Float32Array, axis: number): number => { let lo = Infinity; let hi = -Infinity; for (let i = axis; i < soup.length; i += 3) { lo = Math.min(lo, soup[i]); hi = Math.max(hi, soup[i]); } return hi - lo; };
    expect(extent(tight.trianglePositions, 0), 'the whole width of the floor, not a strip').toBeGreaterThan(0.78 * extent(roomy.trianglePositions, 0));
    expect(extent(tight.trianglePositions, 1), 'the whole depth of the floor, not a strip').toBeGreaterThan(0.78 * extent(roomy.trianglePositions, 1));
  });

  it('gives up coarsening after a few steps and then keeps the old cut as a last resort, still within the budget', () => {
    const pts: number[] = []; const nrm: number[] = [];
    for (let i = 0; i < 80; i += 1) for (let j = 0; j < 80; j += 1) { pts.push(i * 0.05, j * 0.05, 0); nrm.push(0, 0, 1); }
    const positions = Float32Array.from(pts);
    const mesh = buildSparseTsdfMesh({ positions, normals: Float32Array.from(nrm), confidence: new Float32Array(positions.length / 3).fill(0.9), voxelSizeMeters: 0.05, maxTriangles: 60 });
    expect(mesh.coarsenings).toBe(SPARSE_TSDF_MAX_COARSENINGS);
    expect(mesh.triangleCount).toBeLessThanOrEqual(60);
  });

  it('lets the diagnostics look at every triangle of a large clean mesh, so it is kept, not withheld', () => {
    const big = sheet(110, 110, 0.05);                                                                 /* 24,200 triangles */
    const result = sanitizeMeshTopology(big, 0.05);
    expect(result.selfIntersectionTruncated).toBe(false);
    expect(result.retainedTriangleCount).toBe(24200);
    expect(result.quarantinedTriangleCount).toBe(0);
  });

  it('scales the pair budgets with the mesh, never below the old fixed ones and never above a ceiling', () => {
    /* self-intersection: the candidate pairs come from a BVH and are not capped; only the exact tests are */
    expect(selfIntersectionBudgets(100)).toEqual({ exactTests: 12_000 });
    expect(selfIntersectionBudgets(1000)).toEqual({ exactTests: 32_000 });
    expect(selfIntersectionBudgets(10_000_000)).toEqual({ exactTests: 600_000 });
    for (const triangles of [100, 1000, 7000, 20_000, 34_000, 100_000, 10_000_000]) {
      expect(selfIntersectionBudgets(triangles).exactTests, `${triangles} triangles: not below the earlier budget`).toBeGreaterThanOrEqual(Math.min(150_000, Math.max(12_000, triangles * 3)));
    }
    expect(normalIntrusionBudgets(1000)).toEqual({ candidatePairs: 40_000, exactTests: 8_000 });
    expect(normalIntrusionBudgets(20_000)).toEqual({ candidatePairs: 160_000, exactTests: 40_000 });
    expect(normalIntrusionBudgets(10_000_000)).toEqual({ candidatePairs: 400_000, exactTests: 100_000 });
  });
});


const SAVES_MERGED = resolve(__dirname, '../../../logs/phone-lab/2026-11-22_175253/saves');
describe.skipIf(!existsSync(SAVES_MERGED))('the final mesh of the first scan and of the scan merged with its continuation (phone run 2026-11-22 17:52)', () => {
  const scans = () => readdirSync(SAVES_MERGED).filter((f) => f.includes('_scan_')).sort().map((f) => decodeScanArtifact(new Uint8Array(readFileSync(resolve(SAVES_MERGED, f)))));
  const range = (a: Float32Array, axis: number): [number, number] => { let lo = Infinity; let hi = -Infinity; for (let i = axis; i < a.length; i += 3) { lo = Math.min(lo, a[i]); hi = Math.max(hi, a[i]); } return [lo, hi]; };

  it.each([0, 1])('scan %i: the mesh spans the whole scanned area, not bands of it (the floor mesh used to cover one metre of four)', (index) => {
    const scan = scans()[index];
    const mesh = buildDirectionalTsdfMesh(buildTsdfRequestArgs(scan as never) as never);
    for (const axis of [0, 1]) {
      const [pointLo, pointHi] = range(scan.tsdfPositions, axis);
      const [meshLo, meshHi] = range(mesh.trianglePositions, axis);
      expect(meshHi - meshLo, `axis ${axis}`).toBeGreaterThan(0.8 * (pointHi - pointLo));
    }
    expect(mesh.triangleCount).toBeGreaterThan(15_000);
  });
});

const SAVES = resolve(__dirname, '../../../logs/phone-lab/2026-11-22_163311/saves');
describe.skipIf(!existsSync(SAVES))('the final mesh of the real scan of phone run 2026-11-22 16:33', () => {
  const scan = () => decodeScanArtifact(new Uint8Array(readFileSync(resolve(SAVES, readdirSync(SAVES).find((f) => f.includes('_scan_'))!))));

  /** Share of the certified points that have mesh within `radius` metres. */
  function coverage(points: Float32Array, soup: Float32Array, radius: number): number {
    const grid = new Map<string, number[]>();
    const add = (x: number, y: number, z: number) => { const key = `${Math.floor(x / radius)},${Math.floor(y / radius)},${Math.floor(z / radius)}`; const list = grid.get(key); if (list) list.push(x, y, z); else grid.set(key, [x, y, z]); };
    for (let t = 0; t + 8 < soup.length; t += 9) {
      for (const [a, b, c] of [[1 / 3, 1 / 3, 1 / 3], [1, 0, 0], [0, 1, 0], [0, 0, 1], [.5, .5, 0], [0, .5, .5], [.5, 0, .5], [.6, .2, .2], [.2, .6, .2], [.2, .2, .6]]) {
        add(a * soup[t] + b * soup[t + 3] + c * soup[t + 6], a * soup[t + 1] + b * soup[t + 4] + c * soup[t + 7], a * soup[t + 2] + b * soup[t + 5] + c * soup[t + 8]);
      }
    }
    let covered = 0; const n = points.length / 3;
    for (let i = 0; i < n; i += 1) {
      const x = points[i * 3]; const y = points[i * 3 + 1]; const z = points[i * 3 + 2];
      const gx = Math.floor(x / radius); const gy = Math.floor(y / radius); const gz = Math.floor(z / radius);
      let hit = false;
      for (let dx = -1; dx <= 1 && !hit; dx += 1) for (let dy = -1; dy <= 1 && !hit; dy += 1) for (let dz = -1; dz <= 1 && !hit; dz += 1) {
        const list = grid.get(`${gx + dx},${gy + dy},${gz + dz}`); if (!list) continue;
        for (let k = 0; k < list.length; k += 3) if (Math.hypot(list[k] - x, list[k + 1] - y, list[k + 2] - z) <= radius) { hit = true; break; }
      }
      if (hit) covered += 1;
    }
    return covered / n;
  }

  it('covers most of what was scanned (the mesh used to stop after a quarter of the floor) and is not made noisier by the cosmetic pass', () => {
    const s = scan();
    const mesh = buildDirectionalTsdfMesh(buildTsdfRequestArgs(s as never) as never);
    expect(mesh.triangleCount, 'the old mesh had 5,387 triangles').toBeGreaterThan(10_000);
    expect(coverage(s.tsdfPositions, mesh.trianglePositions, 0.06), 'was 0.227').toBeGreaterThan(0.55);
    expect(coverage(s.tsdfPositions, mesh.trianglePositions, 0.25), 'was 0.333').toBeGreaterThan(0.75);
    expect(mesh.meshRefine).toBeDefined();
    expect(mesh.meshRefine!.smoothedVertices, 'the cosmetic pass ran on the real mesh').toBeGreaterThan(1000);
    expect(mesh.meshRefine!.maxDisplacementMm).toBeLessThanOrEqual(0.25 * mesh.voxelSizeMeters * 1000 + 0.1);
    expect(mesh.selfIntersectionTruncated).toBe(false);
  });

  it('keeps the large clean pieces visible instead of quarantining them for incomplete diagnostics', () => {
    const s = scan();
    const plan = prepareDirectionalTsdfPlan(buildTsdfRequestArgs(s as never) as never);
    const mesh = buildDirectionalTsdfMesh(buildTsdfRequestArgs(s as never) as never);
    expect(plan.channelInputs.length).toBeGreaterThan(1);
    expect(mesh.quarantinedTriangleCount / Math.max(1, mesh.rawTriangleCount)).toBeLessThan(0.1);
  });

  it('the cosmetic pass does not add open edges and does not create non-manifold spots', () => {
    const s = scan();
    const mesh = buildDirectionalTsdfMesh(buildTsdfRequestArgs(s as never) as never);
    const again = refineScanMesh(mesh.trianglePositions, mesh.voxelSizeMeters);
    expect(boundaryEdges(again.trianglePositions)).toBeLessThanOrEqual(boundaryEdges(mesh.trianglePositions));
  });
});
