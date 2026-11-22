import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, expect, it } from 'vitest';

import { DepthPointCloudAccumulator } from '../../src/xr/depthPointCloudV2';
import { buildLiveMesh, limitLiveMeshInput, LIVE_MESH_MAX_POINTS, LIVE_MESH_MAX_TRIANGLES } from '../../src/xr/liveMeshCore';
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
  it('triangulates a measured plane and hands the triangles over in the WebXR frame (x, z, -y), on the same plane', () => {
    const result = buildLiveMesh(plane(24, 0.04, 0.8));
    expect(result.triangleCount).toBeGreaterThan(200);
    expect(result.trianglesXr.length).toBe(result.triangleCount * 9);
    for (let v = 0; v < result.triangleCount * 3; v += 1) {
      expect(result.trianglesXr[v * 3 + 1]).toBeCloseTo(0.8, 5);                                  /* project Z (height) is WebXR Y */
      expect(result.trianglesXr[v * 3 + 2]).toBeLessThanOrEqual(1e-6);                           /* project Y in [0, 0.92] is WebXR -Z */
      expect(result.trianglesXr[v * 3 + 2]).toBeGreaterThanOrEqual(-0.93);
      expect(result.trianglesXr[v * 3]).toBeGreaterThanOrEqual(-1e-6);
      expect(result.trianglesXr[v * 3]).toBeLessThanOrEqual(0.93);
    }
  });

  it('never exceeds the point and triangle budgets, however many points arrive', () => {
    const big = plane(120, 0.03);                                                                /* 14,400 points */
    const limited = limitLiveMeshInput(big);
    expect(limited.positions.length / 3).toBe(LIVE_MESH_MAX_POINTS);
    expect(limited.normals.length / 3).toBe(LIVE_MESH_MAX_POINTS);
    expect(limited.confidence.length).toBe(LIVE_MESH_MAX_POINTS);
    expect(buildLiveMesh(big).triangleCount).toBeLessThanOrEqual(LIVE_MESH_MAX_TRIANGLES);
    const capped = buildLiveMesh(plane(24, 0.04), 100);                                          /* the triangle cap itself, with a small cap */
    expect(capped.triangleCount).toBe(100);
    expect(capped.trianglesXr.length).toBe(900);
  });

  it('gives nothing for too few or unoriented points instead of a wrong surface', () => {
    expect(buildLiveMesh(plane(2, 0.04)).triangleCount).toBe(0);
    const flat = plane(20, 0.04);
    flat.normals.fill(0);
    expect(buildLiveMesh(flat).triangleCount).toBe(0);
  });
});

const SAVES = resolve(__dirname, '../../../logs/phone-lab/2026-11-22_163311/saves');
describe.skipIf(!existsSync(SAVES))('the live mesh on the real scan of phone run 2026-11-22 16:33', () => {
  it('builds a mesh from the real surface points, inside the scan bounds and fast enough for a worker every 2 seconds', () => {
    const scan = decodeScanArtifact(new Uint8Array(readFileSync(resolve(SAVES, readdirSync(SAVES).find((f) => f.includes('_scan_'))!))));
    const started = performance.now();
    const result = buildLiveMesh({ positions: scan.positions, normals: scan.normals, confidence: scan.confidence, voxelSizeMeters: scan.voxelSizeMeters });
    const elapsed = performance.now() - started;
    expect(result.triangleCount).toBeGreaterThan(1000);
    expect(result.triangleCount).toBeLessThanOrEqual(LIVE_MESH_MAX_TRIANGLES);
    expect(elapsed, 'desktop time; a phone is several times slower, the interval is 1.8 s').toBeLessThan(900);
    const { bounds } = scan as unknown as { bounds: { min: number[]; max: number[] } | null };
    if (bounds !== null) {
      for (let v = 0; v < result.triangleCount * 3; v += 1) {
        const x = result.trianglesXr[v * 3]; const y = -result.trianglesXr[v * 3 + 2]; const z = result.trianglesXr[v * 3 + 1];      /* back to the project frame */
        expect(x).toBeGreaterThanOrEqual(bounds.min[0] - 0.3); expect(x).toBeLessThanOrEqual(bounds.max[0] + 0.3);
        expect(y).toBeGreaterThanOrEqual(bounds.min[1] - 0.3); expect(y).toBeLessThanOrEqual(bounds.max[1] + 0.3);
        expect(z).toBeGreaterThanOrEqual(bounds.min[2] - 0.3); expect(z).toBeLessThanOrEqual(bounds.max[2] + 0.3);
      }
    }
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
