import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { resolve } from 'node:path';
import { performance } from 'node:perf_hooks';
import { describe, expect, it } from 'vitest';

import { buildTsdfRequestArgs } from '../../../src/components/ScanPointCloudViewport';
import { buildDirectionalTsdfMesh } from '../../../src/xr/directionalTsdf';
import { buildLiveMesh } from '../../../src/xr/liveMeshCore';
import { decodeScanArtifact } from '../../../src/xr/scanArtifact';
import { heapMb, median, summarize, writeReport } from './support/stats';
import { surfaceQuality, topologyMetrics } from './support/meshMetrics';
import {
  cornerScene, cubeScene, doorwayScene, noisyScene, parallelWallsScene, planeScene, sphereScene, thinBoardScene, twoObjectsScene,
  type SyntheticSurfaceScene,
} from './support/syntheticSurfaces';

/*
 * Mesh pipeline benchmark of Phase 2 (stage 0 baseline, run against REAL git checkouts of f40b0cec, 4ad30dc3 and the new HEAD): the live mesh build
 * (time, triangles, GPU upload bytes) and the final reconstruction (time, triangles, components, boundary length, self-intersections, precision/recall
 * on scenes with a known surface, and on the local real scan). Only the public API of the modules is used, so it runs unchanged at every checkout.
 * Desktop numbers only.
 */
const REPO = resolve(__dirname, '../../../..');
const LIVE_REPEATS = Number(process.env.PHASE2_LIVE_REPEATS ?? 30);
const FINAL_REPEATS = Number(process.env.PHASE2_FINAL_REPEATS ?? 5);
const VOXEL = 0.048;
const TOLERANCE = 2 * VOXEL * 1.15;

function lcg(seed: number): () => number { let state = seed >>> 0; return () => { state = (Math.imul(state, 1664525) + 1013904223) >>> 0; return state / 4294967296; }; }

/** A room-like cloud of exactly `count` points (floor, two walls, a box) with 5 mm noise; deterministic. */
function roomCloud(count: number): { positions: Float32Array; normals: Float32Array; confidence: Float32Array } {
  const random = lcg(count * 7919 + 13);
  const positions = new Float32Array(count * 3); const normals = new Float32Array(count * 3);
  for (let i = 0; i < count; i += 1) {
    const pick = random();
    let x: number; let y: number; let z: number; let n: [number, number, number];
    if (pick < 0.45) { x = random() * 4; y = random() * 4; z = 0; n = [0, 0, 1]; }
    else if (pick < 0.65) { x = 0; y = random() * 4; z = random() * 2.4; n = [1, 0, 0]; }
    else if (pick < 0.85) { x = random() * 4; y = 0; z = random() * 2.4; n = [0, 1, 0]; }
    else { x = 2 + random() * 0.6; y = 2 + random() * 0.6; z = 0.6; n = [0, 0, 1]; }
    positions[i * 3] = x + (random() - 0.5) * 0.01; positions[i * 3 + 1] = y + (random() - 0.5) * 0.01; positions[i * 3 + 2] = z + (random() - 0.5) * 0.01;
    normals.set(n, i * 3);
  }
  return { positions, normals, confidence: new Float32Array(count).fill(0.9) };
}

function realScanPath(): string | null {
  const directory = resolve(REPO, 'logs/phone-lab/2026-11-26_231509/saves');
  if (!existsSync(directory)) return null;
  const name = readdirSync(directory).find((entry) => /_scan_\d+\.bin$/.test(entry));
  return name === undefined ? null : resolve(directory, name);
}

describe('live mesh build (CPU part), identical clouds at every checkout', () => {
  it(`${LIVE_REPEATS} repeats per size: build time, triangles and GPU upload bytes`, () => {
    const report: Record<string, unknown> = {};
    const sizes = [1000, 2000, 4000, 6000, 10000];
    const inputs: Array<[string, { positions: Float32Array; normals: Float32Array; confidence: Float32Array }]> = sizes.map((n) => [`room-${n}`, roomCloud(n)]);
    const path = realScanPath();
    if (path !== null) {
      const snap = decodeScanArtifact(new Uint8Array(readFileSync(path)));
      inputs.push(['real-scan-stable', { positions: snap.positions, normals: snap.normals, confidence: snap.confidence }]);
    }
    for (const [name, cloud] of inputs) {
      const input = { ...cloud, voxelSizeMeters: 0.04 };
      for (let i = 0; i < 6; i += 1) buildLiveMesh(input);
      const samples: number[] = []; let triangles = 0; let source = 0;
      const heapStart = heapMb();
      for (let i = 0; i < LIVE_REPEATS; i += 1) { const t0 = performance.now(); const result = buildLiveMesh(input); samples.push(performance.now() - t0); triangles = result.triangleCount; source = result.sourcePointCount; }
      report[name] = { buildMs: summarize(samples), triangles, sourcePoints: source, uploadBytes: triangles * 9 * 4, heapDeltaMb: heapMb() - heapStart };
      expect(triangles, name).toBeGreaterThan(0);
    }
    writeReport('mesh', 'liveMesh', report);
  }, 600_000);
});

interface FinalResult { ms: number; triangles: number; components: number; boundaryM: number; degenerate: number; nonManifold: number; selfIntersections: number; precision?: number; recall?: number }

function runFinal(positions: Float32Array, normals: Float32Array, confidence: Float32Array, voxel: number, maxTriangles: number, truth?: Float32Array): FinalResult {
  const t0 = performance.now();
  const mesh = buildDirectionalTsdfMesh({ positions, normals, confidence, voxelSizeMeters: voxel, maxTriangles, maxInputPoints: 12000 } as never);
  const ms = performance.now() - t0;
  const topology = topologyMetrics(mesh.trianglePositions);
  const quality = truth === undefined ? {} : (() => { const q = surfaceQuality(mesh.trianglePositions, truth, TOLERANCE); return { precision: q.precision, recall: q.recall }; })();
  return { ms, triangles: mesh.triangleCount, components: topology.components, boundaryM: topology.boundaryEdgeLengthMeters, degenerate: topology.degenerateTriangles, nonManifold: topology.nonManifoldEdges, selfIntersections: (mesh as { selfIntersectionPairCount?: number }).selfIntersectionPairCount ?? -1, ...quality };
}

const SCENES: Array<[string, () => SyntheticSurfaceScene]> = [
  ['plane', planeScene], ['corner', cornerScene], ['cube', cubeScene], ['sphere', sphereScene], ['parallelWalls', parallelWallsScene], ['doorway', doorwayScene],
  ['twoObjects', twoObjectsScene], ['noisyCorner', () => noisyScene()], ['thinBoard', thinBoardScene],
];

describe('final reconstruction (directional TSDF entry point), identical inputs at every checkout', () => {
  it(`${FINAL_REPEATS} repeats per synthetic scene with a known surface: time, topology, precision and recall`, () => {
    const report: Record<string, unknown> = {};
    const empty: string[] = [];
    for (const [name, make] of SCENES) {
      const scene = make();
      const confidence = new Float32Array(scene.points.length / 3).fill(0.9);
      runFinal(scene.points, scene.normals, confidence, VOXEL, 40000, scene.truth);                                  /* warm-up */
      const runs = Array.from({ length: FINAL_REPEATS }, () => runFinal(scene.points, scene.normals, confidence, VOXEL, 40000, scene.truth));
      const last = runs[runs.length - 1];
      report[name] = { ms: summarize(runs.map((r) => r.ms)), triangles: last.triangles, components: last.components, boundaryM: last.boundaryM, degenerate: last.degenerate, nonManifold: last.nonManifold, selfIntersections: last.selfIntersections, precision: last.precision, recall: last.recall };
      empty.push(...(last.triangles === 0 ? [name] : []));
    }
    writeReport('mesh', 'finalSynthetic', { ...report, _scenesWithNoTriangles: empty });
    if (process.env.PHASE2_STRICT === '1') expect(empty, 'every scene must produce a mesh').toEqual([]);
  }, 1_800_000);

  it(`${FINAL_REPEATS} repeats on the local real scan (original, not decimated)`, () => {
    const path = realScanPath();
    expect(path, 'the original local scan files must exist (never skipped)').not.toBeNull();
    const snapshot = decodeScanArtifact(new Uint8Array(readFileSync(path!)));
    const args = buildTsdfRequestArgs(snapshot as never);
    const run = () => runFinal(args.positions, args.normals, args.confidence, args.voxelSizeMeters, args.maxTriangles);
    run();
    const runs = Array.from({ length: FINAL_REPEATS }, run);
    const last = runs[runs.length - 1];
    writeReport('mesh', 'finalRealScan', { sourcePoints: args.positions.length / 3, ms: summarize(runs.map((r) => r.ms)), medianMs: median(runs.map((r) => r.ms)), triangles: last.triangles, components: last.components, boundaryM: last.boundaryM, degenerate: last.degenerate, nonManifold: last.nonManifold, selfIntersections: last.selfIntersections });
    expect(last.triangles).toBeGreaterThan(1000);
  }, 1_800_000);
});
