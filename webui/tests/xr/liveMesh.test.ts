import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, expect, it } from 'vitest';

import { DepthPointCloudAccumulator } from '../../src/xr/depthPointCloudV2';
import {
  buildLiveMesh, limitLiveMeshInput, LIVE_MESH_CELL_VOXELS, LIVE_MESH_MAX_CELL_METERS, LIVE_MESH_MAX_POINTS, LIVE_MESH_MAX_TRIANGLES, LIVE_MESH_MIN_CELL_METERS,
} from '../../src/xr/liveMeshCore';
import {
  LiveMeshClient, LIVE_MESH_MAX_CONSECUTIVE_FAILURES, LIVE_MESH_MIN_INTERVAL_MS,
  type LiveMeshWorkerLike,
} from '../../src/xr/liveMeshClient';
import type { LiveMeshInput, LiveMeshResult } from '../../src/xr/liveMeshCore';
import type { LiveMeshRequest, LiveMeshResponse } from '../../src/xr/liveMeshWorker';
import { decodeScanArtifact } from '../../src/xr/scanArtifact';

/** A horizontal plane (project frame, Z up) sampled on a grid, normals straight up. */
function plane(side: number, spacing: number, z = 0.8): LiveMeshInput {
  const points: number[] = []; const normals: number[] = [];
  for (let i = 0; i < side; i += 1) for (let j = 0; j < side; j += 1) { points.push(i * spacing, j * spacing, z); normals.push(0, 0, 1); }
  return { positions: Float32Array.from(points), normals: Float32Array.from(normals), confidence: new Float32Array(side * side).fill(0.9), voxelSizeMeters: 0.04 };
}

describe('the live mesh core', () => {
  const cellOf = (voxel: number): number => Math.min(LIVE_MESH_MAX_CELL_METERS, Math.max(LIVE_MESH_MIN_CELL_METERS, voxel * LIVE_MESH_CELL_VOXELS));

  it('hands a measured plane over as one skin in the WebXR frame (x, z, -y), near the plane', () => {
    const result = buildLiveMesh(plane(24, 0.04, 0.8));
    const cell = cellOf(0.04);
    expect(result.triangleCount).toBeGreaterThan(100);
    expect(result.trianglesXr.length).toBe(result.triangleCount * 9);
    expect(result.acceptedPointCount, 'occupied lattice cells').toBeGreaterThan(50);
    for (let v = 0; v < result.triangleCount * 3; v += 1) {
      expect(Math.abs(result.trianglesXr[v * 3 + 1] - 0.8), 'project Z (height) is WebXR Y').toBeLessThanOrEqual(cell);
      expect(result.trianglesXr[v * 3 + 2]).toBeLessThanOrEqual(cell + 1e-6);                     /* project Y in [0, 0.92] is WebXR -Z */
      expect(result.trianglesXr[v * 3 + 2]).toBeGreaterThanOrEqual(-(0.92 + cell) - 1e-6);
      expect(result.trianglesXr[v * 3]).toBeGreaterThanOrEqual(-cell - 1e-6);
      expect(result.trianglesXr[v * 3]).toBeLessThanOrEqual(0.92 + cell + 1e-6);
    }
  });

  it('puts a wall where it is: a vertical surface stays vertical, at the right distance, after the frame change', () => {
    const points: number[] = []; const normals: number[] = [];
    for (let i = 0; i < 30; i += 1) for (let k = 0; k < 20; k += 1) { points.push(1 + i * 0.04, 2, 0.2 + k * 0.04); normals.push(0, 1, 0); }
    const result = buildLiveMesh({ positions: Float32Array.from(points), normals: Float32Array.from(normals), confidence: new Float32Array(600).fill(0.9), voxelSizeMeters: 0.04 });
    expect(result.triangleCount).toBeGreaterThan(50);
    const cell = cellOf(0.04);
    let lowY = Infinity; let highY = -Infinity;
    for (let v = 0; v < result.triangleCount * 3; v += 1) {
      expect(Math.abs(-result.trianglesXr[v * 3 + 2] - 2), 'WebXR -Z is project Y: the wall is at Y = 2').toBeLessThanOrEqual(cell);
      lowY = Math.min(lowY, result.trianglesXr[v * 3 + 1]); highY = Math.max(highY, result.trianglesXr[v * 3 + 1]);
    }
    expect(lowY).toBeLessThan(0.2 + cell); expect(highY).toBeGreaterThan(0.2 + 19 * 0.04 - cell);       /* spans the wall's height (WebXR Y) */
  });

  it('never exceeds the point and triangle budgets, however many points arrive: it coarsens the cells instead of cutting', () => {
    const big = plane(120, 0.03);                                                                /* 14,400 points */
    const limited = limitLiveMeshInput(big);
    expect(limited.positions.length / 3).toBe(LIVE_MESH_MAX_POINTS);
    expect(limited.normals.length / 3).toBe(LIVE_MESH_MAX_POINTS);
    expect(limited.confidence.length).toBe(LIVE_MESH_MAX_POINTS);
    const roomy = buildLiveMesh(big);
    expect(roomy.triangleCount).toBeLessThanOrEqual(LIVE_MESH_MAX_TRIANGLES);
    const tight = buildLiveMesh(big, 1500);
    expect(tight.triangleCount).toBeLessThanOrEqual(1500);
    expect(tight.triangleCount, 'a coarser skin over the whole plane, not the first 1500 triangles of a finer one').toBeGreaterThan(300);
    const extent = (soup: Float32Array, axis: number): number => { let lo = Infinity; let hi = -Infinity; for (let i = axis; i < soup.length; i += 3) { lo = Math.min(lo, soup[i]); hi = Math.max(hi, soup[i]); } return hi - lo; };
    expect(extent(tight.trianglesXr, 0)).toBeGreaterThan(0.8 * extent(roomy.trianglesXr, 0));
    expect(extent(tight.trianglesXr, 2)).toBeGreaterThan(0.8 * extent(roomy.trianglesXr, 2));
    const capped = buildLiveMesh(plane(24, 0.04), 100);                                          /* last resort when even the coarsest cell does not fit */
    expect(capped.triangleCount).toBeLessThanOrEqual(100);
  });

  it('gives nothing for too few points or non-finite ones, and never a non-finite coordinate', () => {
    expect(buildLiveMesh(plane(2, 0.04)).triangleCount).toBe(0);
    const damaged = plane(20, 0.04);
    damaged.positions[5] = Number.NaN;
    damaged.positions[30] = Infinity;
    const result = buildLiveMesh(damaged);
    expect(result.triangleCount).toBeGreaterThan(100);
    expect(Array.from(result.trianglesXr).every(Number.isFinite)).toBe(true);
  });
});

const SAVES = resolve(__dirname, '../../../logs/phone-lab/2026-11-22_163311/saves');
describe.skipIf(!existsSync(SAVES))('the live mesh on the real scan of phone run 2026-11-22 16:33', () => {
  it('is one connected skin over almost all measured points, built in milliseconds (the earlier versions: 31 to 56 islands, or a screen of squares)', () => {
    const scan = decodeScanArtifact(new Uint8Array(readFileSync(resolve(SAVES, readdirSync(SAVES).find((f) => f.includes('_scan_'))!))));
    const started = performance.now();
    const result = buildLiveMesh({ positions: scan.positions, normals: scan.normals, confidence: scan.confidence, voxelSizeMeters: scan.voxelSizeMeters });
    const elapsed = performance.now() - started;
    expect(result.triangleCount).toBeGreaterThan(3000);
    expect(result.triangleCount).toBeLessThanOrEqual(LIVE_MESH_MAX_TRIANGLES);
    expect(elapsed, 'a phone is several times slower; the interval is seconds').toBeLessThan(200);
    /* back to the project frame for the geometry checks */
    const project = new Float32Array(result.trianglesXr.length);
    for (let v = 0; v < result.triangleCount * 3; v += 1) { project[v * 3] = result.trianglesXr[v * 3]; project[v * 3 + 1] = -result.trianglesXr[v * 3 + 2]; project[v * 3 + 2] = result.trianglesXr[v * 3 + 1]; }
    const ids = new Map<string, number>(); const parent: number[] = [];
    const find = (x: number): number => { while (parent[x] !== x) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; };
    const id = (o: number): number => { const k = `${project[o].toFixed(4)},${project[o + 1].toFixed(4)},${project[o + 2].toFixed(4)}`; let v = ids.get(k); if (v === undefined) { v = ids.size; ids.set(k, v); parent.push(v); } return v; };
    const first: number[] = [];
    for (let t = 0; t < result.triangleCount; t += 1) { const a = id(t * 9); const b = id(t * 9 + 3); const c = id(t * 9 + 6); parent[find(a)] = find(b); parent[find(b)] = find(c); first.push(a); }
    const sizes = new Map<number, number>();
    for (const f of first) sizes.set(find(f), (sizes.get(find(f)) ?? 0) + 1);
    const largest = Math.max(...sizes.values());
    expect(largest / result.triangleCount, 'one main connected surface').toBeGreaterThan(0.85);
    expect(sizes.size, 'not dozens of islands').toBeLessThan(25);
    /* coverage: the share of the (thinned) measured points that have a mesh vertex within two cells */
    const limited = limitLiveMeshInput({ positions: scan.positions, normals: scan.normals, confidence: scan.confidence, voxelSizeMeters: scan.voxelSizeMeters });
    const cell = Math.min(LIVE_MESH_MAX_CELL_METERS, Math.max(LIVE_MESH_MIN_CELL_METERS, scan.voxelSizeMeters * LIVE_MESH_CELL_VOXELS));
    const grid = new Map<string, number[]>();
    for (let v = 0; v < result.triangleCount * 3; v += 1) { const k = `${Math.floor(project[v * 3] / cell)},${Math.floor(project[v * 3 + 1] / cell)},${Math.floor(project[v * 3 + 2] / cell)}`; const l = grid.get(k); if (l) l.push(v); else grid.set(k, [v]); }
    let covered = 0; const n = limited.confidence.length;
    for (let i = 0; i < n; i += 1) {
      const x = limited.positions[i * 3]; const y = limited.positions[i * 3 + 1]; const z = limited.positions[i * 3 + 2];
      let hit = false;
      for (let dx = -2; dx <= 2 && !hit; dx += 1) for (let dy = -2; dy <= 2 && !hit; dy += 1) for (let dz = -2; dz <= 2 && !hit; dz += 1) {
        const l = grid.get(`${Math.floor(x / cell) + dx},${Math.floor(y / cell) + dy},${Math.floor(z / cell) + dz}`); if (!l) continue;
        for (const v of l) if (Math.hypot(project[v * 3] - x, project[v * 3 + 1] - y, project[v * 3 + 2] - z) <= 2 * cell) { hit = true; break; }
      }
      if (hit) covered += 1;
    }
    expect(covered / n).toBeGreaterThan(0.9);
  });
});

class FakeWorker implements LiveMeshWorkerLike {
  onmessage: ((event: { data: LiveMeshResponse }) => void) | null = null;
  onerror: ((event: unknown) => void) | null = null;
  posted: Array<{ message: LiveMeshRequest; transfer: Transferable[] | undefined }> = [];
  terminated = false;
  postMessage(message: LiveMeshRequest, transfer?: Transferable[]): void { this.posted.push({ message, transfer }); }
  terminate(): void { this.terminated = true; }
  reply(response: LiveMeshResponse): void { this.onmessage?.({ data: response }); }
}
const okResult = (): LiveMeshResult => ({ trianglesXr: new Float32Array(9), triangleCount: 1, sourcePointCount: 3, acceptedPointCount: 3 });
const input = (): LiveMeshInput => plane(4, 0.04);

describe('the live mesh client keeps the scan first', () => {
  function setup(extra: { minIntervalMs?: number } = {}) {
    const worker = new FakeWorker();
    let time = 10_000;
    const results: LiveMeshResult[] = [];
    const disabled: string[] = [];
    const client = new LiveMeshClient({ createWorker: () => worker, now: () => time, onResult: (r) => results.push(r), onDisabled: (reason) => disabled.push(reason), ...extra });
    return { worker, client, results, disabled, advance: (ms: number) => { time += ms; } };
  }

  it('builds one mesh at a time and queues nothing: a request during a build is refused and does not even gather points', () => {
    const { client, worker, advance } = setup();
    let gathered = 0;
    const gather = () => { gathered += 1; return input(); };
    expect(client.request(gather)).toBe(true);
    advance(LIVE_MESH_MIN_INTERVAL_MS * 3);
    expect(client.request(gather)).toBe(false);                                                   /* the first is still running */
    expect(gathered).toBe(1);
    expect(worker.posted).toHaveLength(1);
    worker.reply({ type: 'result', requestId: worker.posted[0].message.requestId, result: okResult(), elapsedMs: 5 });
    expect(client.request(gather)).toBe(true);
    expect(worker.posted).toHaveLength(2);
  });

  it('never starts a build sooner than the interval after the last one', () => {
    const { client, worker, advance } = setup();
    expect(client.request(input)).toBe(true);
    worker.reply({ type: 'result', requestId: 1, result: okResult(), elapsedMs: 1 });
    advance(LIVE_MESH_MIN_INTERVAL_MS - 1);
    expect(client.request(input)).toBe(false);
    advance(1);
    expect(client.request(input)).toBe(true);
  });

  it('transfers the input buffers instead of copying them, and delivers each result once', () => {
    const { client, worker, results } = setup();
    const data = input();
    client.request(() => data);
    expect(worker.posted[0].transfer).toEqual([data.positions.buffer, data.normals.buffer, data.confidence.buffer]);
    const requestId = worker.posted[0].message.requestId;
    worker.reply({ type: 'result', requestId, result: okResult(), elapsedMs: 5 });
    worker.reply({ type: 'result', requestId, result: okResult(), elapsedMs: 5 });                 /* a duplicate or stale reply */
    expect(results).toHaveLength(1);
  });

  it('switches itself off after repeated failures, so a broken preview can never cost the scan', () => {
    const { client, worker, disabled, advance } = setup();
    for (let i = 0; i < LIVE_MESH_MAX_CONSECUTIVE_FAILURES; i += 1) {
      advance(LIVE_MESH_MIN_INTERVAL_MS);
      expect(client.request(input)).toBe(true);
      worker.reply({ type: 'error', requestId: worker.posted[i].message.requestId, message: 'boom' });
    }
    expect(disabled).toEqual(['boom']);
    advance(LIVE_MESH_MIN_INTERVAL_MS);
    expect(client.request(input)).toBe(false);
  });

  it('a success in between resets the failure count', () => {
    const { client, worker, disabled, advance } = setup();
    const answer = (index: number, response: LiveMeshResponse) => { worker.reply({ ...response, requestId: worker.posted[index].message.requestId } as LiveMeshResponse); };
    for (let i = 0; i < 6; i += 1) {
      advance(LIVE_MESH_MIN_INTERVAL_MS);
      client.request(input);
      answer(i, i % 3 === 2 ? { type: 'result', requestId: 0, result: okResult(), elapsedMs: 1 } : { type: 'error', requestId: 0, message: 'x' });
    }
    expect(disabled).toEqual([]);
  });

  it('terminates the worker on dispose and ignores a late result', () => {
    const { client, worker, results } = setup();
    client.request(input);
    client.dispose();
    expect(worker.terminated).toBe(true);
    worker.reply({ type: 'result', requestId: worker.posted[0].message.requestId, result: okResult(), elapsedMs: 1 });
    expect(results).toEqual([]);
    expect(client.request(input)).toBe(false);
  });

  it('does not build a worker when there is nothing to mesh yet', () => {
    let created = 0;
    const client = new LiveMeshClient({ createWorker: () => { created += 1; return new FakeWorker(); }, onResult: () => undefined });
    expect(client.request(() => null)).toBe(false);
    expect(created).toBe(0);
  });
});

describe('the live surface handed to the mesh', () => {
  function accumulatorWith(pointsPerCell: number, cells: number, normalKnown: boolean | ((cell: number) => boolean) = true): DepthPointCloudAccumulator {
    const accumulator = new DepthPointCloudAccumulator();
    const internal = accumulator as unknown as { coverageCells: Map<string, unknown>; representativeCount: number; integrationKeyframes: number };
    internal.integrationKeyframes = 2;
    for (let c = 0; c < cells; c += 1) {
      const representatives = new Map<string, unknown>();
      for (let p = 0; p < pointsPerCell; p += 1) {
        const id = `${c}:${p}`;
        representatives.set(id, {
          id, x: c * 0.1 + p * 0.01, y: p * 0.02, z: (typeof normalKnown === 'function' && !normalKnown(c)) ? 0.9 : 0.5, normalX: 0, normalY: 0, normalZ: 1, normalConfidence: 0.9, normalKnown: typeof normalKnown === 'function' ? normalKnown(c) : normalKnown,
          weight: 5, acceptedSamples: 8, independentViews: 4, viewCodesPacked: 0, viewCodeCount: 4, lastProcessedKeyframe: 2, lastEvidenceKeyframe: 2,
          lastSeenEpoch: 2, lastEvidenceEpoch: 2, varianceX: 1e-6, varianceY: 1e-6, varianceZ: 1e-6, residualEwma: 0.001, viewDirX: 0, viewDirY: 0, viewDirZ: -1,
          maxViewAngleRadians: 0.7, contradictionWeight: 0, promoted: true, priority: 0.6, voxelKey: id, voxelIndexKey: id, coverageKey: String(c),
        });
      }
      internal.coverageCells.set(String(c), { representatives, lastEvidenceKeyframe: 2, evidenceKeyframes: 2 });
      internal.representativeCount += pointsPerCell;
    }
    return accumulator;
  }

  it('is null while nothing with a measured orientation exists', () => {
    expect(new DepthPointCloudAccumulator().liveMeshInput(4000)).toBeNull();
    expect(accumulatorWith(10, 5, false).liveMeshInput(4000)).toBeNull();
  });

  it('leaves out the points whose orientation was never measured, even among measured ones', () => {
    const accumulator = accumulatorWith(20, 40, (cell) => cell % 2 === 0);                        /* odd cells: orientation unknown, marked by z = 0.9 */
    const live = accumulator.liveMeshInput(4000)!;
    expect(live.positions.length / 3).toBe(400);
    for (let i = 0; i < live.positions.length / 3; i += 1) expect(live.positions[i * 3 + 2]).toBeCloseTo(0.5, 6);
  });

  it('returns aligned project-frame arrays with the measured normals, thinned evenly to the budget', () => {
    const accumulator = accumulatorWith(40, 200);                                                /* 8,000 points */
    const live = accumulator.liveMeshInput(1000)!;
    expect(live.positions.length / 3).toBe(1000);
    expect(live.normals.length / 3).toBe(1000);
    expect(live.confidence.length).toBe(1000);
    expect(live.positions[2]).toBeCloseTo(0.5, 6);                                               /* project Z stays Z: no frame change here */
    expect(live.normals[2]).toBeCloseTo(1, 6);
    expect(live.voxelSizeMeters).toBeGreaterThan(0);
    const xs = Array.from({ length: 1000 }, (_, i) => live.positions[i * 3]);
    expect(Math.max(...xs) - Math.min(...xs), 'thinned over all cells, not just the first ones').toBeGreaterThan(15);
  });
});
