import { mkdirSync, writeFileSync } from 'node:fs';
import { cpus } from 'node:os';
import { dirname, resolve } from 'node:path';
import { performance } from 'node:perf_hooks';

import { describe, expect, it } from 'vitest';

import { buildLiveMesh, LIVE_MESH_MAX_POINTS } from '../../src/xr/liveMeshCore';
import { buildSurfaceNets } from '../../src/xr/liveSurfaceNets';
import { buildSurfaceNets as buildBaselineSurfaceNets } from './baselineLiveSurfaceNets';

/*
 * TASK 08 benchmark of the live mesh build (the part of the live mesh that runs on a CPU). Same fixtures, same process, interleaved, warmed up and repeated:
 * the surface-nets build of commit f40b0cec (frozen in baselineLiveSurfaceNets.ts) against the current one. Gate: P95 of the current build is not more than
 * 10% worse than the baseline on the identical fixture. The numbers are DESKTOP numbers (this Node process); they say nothing about a phone.
 */
const SIZES = [1000, 2000, 4000, 6000, 10000] as const;
const WARMUP = 6;
const RUNS = 100;
const ROUNDS = 5;
const GATE = 1.10;

function lcg(seed: number): () => number { let state = seed >>> 0; return () => { state = (Math.imul(state, 1664525) + 1013904223) >>> 0; return state / 4294967296; }; }

/** A room-like cloud of exactly `count` points: a floor, two walls and a box, sampled with 5 mm noise. Deterministic. */
function roomCloud(count: number): { positions: Float32Array; normals: Float32Array; confidence: Float32Array } {
  const random = lcg(count * 7919 + 13);
  const positions = new Float32Array(count * 3); const normals = new Float32Array(count * 3);
  for (let i = 0; i < count; i += 1) {
    const pick = random();
    let x: number; let y: number; let z: number; let n: [number, number, number];
    if (pick < 0.45) { x = random() * 4; y = random() * 4; z = 0; n = [0, 0, 1]; }
    else if (pick < 0.65) { x = 0; y = random() * 4; z = random() * 2.4; n = [1, 0, 0]; }
    else if (pick < 0.85) { x = random() * 4; y = 0; z = random() * 2.4; n = [0, 1, 0]; }
    else { const face = Math.floor(random() * 3); x = 2 + random() * 0.6; y = 2 + random() * 0.6; z = random() * 0.6; if (face === 0) { z = 0.6; n = [0, 0, 1]; } else if (face === 1) { x = 2; n = [-1, 0, 0]; } else { y = 2; n = [0, -1, 0]; } }
    positions[i * 3] = x + (random() - 0.5) * 0.01; positions[i * 3 + 1] = y + (random() - 0.5) * 0.01; positions[i * 3 + 2] = z + (random() - 0.5) * 0.01;
    normals.set(n, i * 3);
  }
  return { positions, normals, confidence: new Float32Array(count).fill(0.9) };
}

function quantile(sorted: number[], q: number): number { return sorted[Math.min(sorted.length - 1, Math.max(0, Math.ceil(q * sorted.length) - 1))]; }
function summarize(samples: number[]): { p50: number; p95: number; p99: number; mean: number } {
  const sorted = samples.slice().sort((a, b) => a - b);
  return { p50: quantile(sorted, 0.5), p95: quantile(sorted, 0.95), p99: quantile(sorted, 0.99), mean: samples.reduce((a, b) => a + b, 0) / samples.length };
}
const median = (values: number[]): number => values.slice().sort((a, b) => a - b)[Math.floor(values.length / 2)];
const time = (fn: () => unknown): number => { const t0 = performance.now(); fn(); return performance.now() - t0; };

describe('live mesh build benchmark (desktop Node, interleaved against the baseline of f40b0cec)', () => {
  const report: Record<string, unknown> = { machine: { cpu: cpus()[0]?.model ?? 'unknown', logicalProcessors: cpus().length, node: process.version }, kind: 'DESKTOP (Node), not a phone', gate: `current P95 <= ${GATE} x baseline P95`, sizes: {} };

  for (const size of SIZES) {
    it(`${size} points: P95 of the current surface-nets build is within ${Math.round((GATE - 1) * 100)}% of the baseline`, () => {
      const cloud = roomCloud(size);
      const cell = 0.08;
      const run = (variant: 'baseline' | 'current') => (variant === 'baseline' ? buildBaselineSurfaceNets : buildSurfaceNets)({ positions: cloud.positions, pointCount: size, cellMeters: cell });
      for (let i = 0; i < WARMUP; i += 1) { run('baseline'); run('current'); }
      const rounds: Array<{ baseline: ReturnType<typeof summarize>; current: ReturnType<typeof summarize> }> = [];
      for (let round = 0; round < ROUNDS; round += 1) {
        const baseline: number[] = []; const current: number[] = [];
        for (let i = 0; i < RUNS; i += 1) {
          /* alternate which goes first so neither always pays for the other's garbage */
          if (i % 2 === 0) { baseline.push(time(() => run('baseline'))); current.push(time(() => run('current'))); }
          else { current.push(time(() => run('current'))); baseline.push(time(() => run('baseline'))); }
        }
        rounds.push({ baseline: summarize(baseline), current: summarize(current) });
      }
      const baselineP95 = median(rounds.map((r) => r.baseline.p95));
      const currentP95 = median(rounds.map((r) => r.current.p95));
      const triangles = { baseline: run('baseline').triangleCount, current: run('current').triangleCount };
      (report.sizes as Record<string, unknown>)[String(size)] = {
        surfaceNets: { baselineP50: median(rounds.map((r) => r.baseline.p50)), currentP50: median(rounds.map((r) => r.current.p50)), baselineP95, currentP95, baselineP99: median(rounds.map((r) => r.baseline.p99)), currentP99: median(rounds.map((r) => r.current.p99)), ratioP95: currentP95 / baselineP95 },
        triangles,
      };
      expect(triangles.current, 'same geometry (the winding fix only reorders corners)').toBe(triangles.baseline);
      expect(currentP95 / baselineP95, `P95 current ${currentP95.toFixed(2)} ms vs baseline ${baselineP95.toFixed(2)} ms`).toBeLessThanOrEqual(GATE);
    });
  }

  it('the whole live build (limit + surface nets + frame conversion) at every size, and at the supported maximum', () => {
    const whole: Record<string, unknown> = {};
    for (const size of [...SIZES, LIVE_MESH_MAX_POINTS]) {
      const cloud = roomCloud(size);
      const input = { positions: cloud.positions, normals: cloud.normals, confidence: cloud.confidence, voxelSizeMeters: 0.04 };
      for (let i = 0; i < WARMUP; i += 1) buildLiveMesh(input);
      const samples: number[] = [];
      let triangles = 0;
      for (let i = 0; i < RUNS * 2; i += 1) { const t0 = performance.now(); triangles = buildLiveMesh(input).triangleCount; samples.push(performance.now() - t0); }
      whole[String(size)] = { ...summarize(samples), triangles, note: size > LIVE_MESH_MAX_POINTS ? `input is limited to ${LIVE_MESH_MAX_POINTS} points before the build` : 'not limited' };
      expect(triangles).toBeGreaterThan(0);
    }
    report.wholeBuild = whole;
    const out = resolve(__dirname, '../../../.test-build/bench/live-mesh-build.json');
    mkdirSync(dirname(out), { recursive: true });
    writeFileSync(out, JSON.stringify(report, null, 2));
  });
});
