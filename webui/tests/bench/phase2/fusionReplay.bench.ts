import { existsSync } from 'node:fs';
import { performance } from 'node:perf_hooks';
import { describe, expect, it } from 'vitest';

import { DepthPointCloudAccumulator } from '../../../src/xr/depthPointCloud';
import type { DepthFusionFrameContext } from '../../../src/xr/depthPointCloudV2';
import {
  ARCORE_NOISE, boxCornerScene, DEFAULT_INTRINSICS, insideCornerScene, nearestPolygon, orbitPoses, renderDepth, stepScene, tiltedPlaneScene,
  type SyntheticScene,
} from '../../xr/support/syntheticDepthScene';
import { DATASET_ROOT, depthInformation, findSequenceDir, loadIclSequence, loadRedwoodSequence, loadTumLike, projectCameraPosition, TUM_INTRINSICS, type ReplayFrame } from './support/replay';
import { heapMb, summarize, writeReport } from './support/stats';

/*
 * Fusion workload replay (Phase 2, stage 0 baseline): the cost of DepthPointCloudAccumulator.sampleDepthFrame, the call the XR frame loop makes
 * synchronously (webxrRuntime.ts), and the quality of what it keeps. Warm-up and repeats follow the TASK 08 rules: >= 20 repeats of the short synthetic
 * scans, >= 5 of the long dataset replays. Desktop numbers only.
 */
const WARMUP_CALLS = 8;
const SYNTHETIC_REPEATS = Number(process.env.PHASE2_SYNTHETIC_REPEATS ?? 20);
const DATASET_REPEATS = Number(process.env.PHASE2_DATASET_REPEATS ?? 5);
const TOLERANCE = 0.03;

const SCENES: Array<[string, () => SyntheticScene]> = [
  ['insideCorner', () => insideCornerScene([0, 0, 0], 0.6)],
  ['boxCorner', () => boxCornerScene([0, 0, 0], 0.4)],
  ['step', () => stepScene(0.2, 0.6)],
  ['tilted30', () => tiltedPlaneScene(30, 20, 0.8, [0, 0, 0])],
];

type P3 = [number, number, number];

function sceneTargets(scene: SyntheticScene): P3[] {
  const points: P3[] = [];
  for (const polygon of scene.polygons) {
    const [a, b, c] = polygon.vertices;
    const u = [b[0] - a[0], b[1] - a[1], b[2] - a[2]];
    const v = [c[0] - b[0], c[1] - b[1], c[2] - b[2]];
    const nu = Math.max(1, Math.ceil(Math.hypot(u[0], u[1], u[2]) / 0.02));
    const nv = Math.max(1, Math.ceil(Math.hypot(v[0], v[1], v[2]) / 0.02));
    for (let i = 0; i <= nu; i += 1) for (let j = 0; j <= nv; j += 1) {
      const s = i / nu; const t = j / nv;
      points.push([a[0] + u[0] * s + v[0] * t, a[1] + u[1] * s + v[1] * t, a[2] + u[2] * s + v[2] * t]);
    }
  }
  return points;
}

function recallPrecision(scene: SyntheticScene, positions: Float32Array): { recall: number; precision: number; points: number } {
  const cell = TOLERANCE;
  const grid = new Map<string, number[]>();
  const key = (x: number, y: number, z: number): string => `${Math.floor(x / cell)},${Math.floor(y / cell)},${Math.floor(z / cell)}`;
  for (let i = 0; i + 2 < positions.length; i += 3) {
    const k = key(positions[i], positions[i + 1], positions[i + 2]);
    const list = grid.get(k);
    if (list) list.push(i); else grid.set(k, [i]);
  }
  const near = (p: P3): boolean => {
    const cx = Math.floor(p[0] / cell); const cy = Math.floor(p[1] / cell); const cz = Math.floor(p[2] / cell);
    for (let dx = -1; dx <= 1; dx += 1) for (let dy = -1; dy <= 1; dy += 1) for (let dz = -1; dz <= 1; dz += 1) {
      for (const i of grid.get(`${cx + dx},${cy + dy},${cz + dz}`) ?? []) if (Math.hypot(positions[i] - p[0], positions[i + 1] - p[1], positions[i + 2] - p[2]) <= TOLERANCE) return true;
    }
    return false;
  };
  const targets = sceneTargets(scene);
  const recall = targets.filter(near).length / Math.max(1, targets.length);
  let good = 0;
  const total = positions.length / 3;
  for (let i = 0; i + 2 < positions.length; i += 3) if (nearestPolygon(scene, [positions[i], positions[i + 1], positions[i + 2]]).distance <= TOLERANCE) good += 1;
  return { recall, precision: total === 0 ? 0 : good / total, points: total };
}

/** Wraps the occupancy integration of one accumulator so that its share of every sampleDepthFrame call can be reported (surface fusion = total - occupancy). */
function instrumentOccupancy(accumulator: DepthPointCloudAccumulator): { take: () => number } {
  const occupancy = (accumulator as unknown as { occupancy: { integrateDepthFrame: (...args: unknown[]) => unknown } }).occupancy;
  const original = occupancy.integrateDepthFrame.bind(occupancy);
  let accumulated = 0;
  occupancy.integrateDepthFrame = (...args: unknown[]) => { const t0 = performance.now(); try { return original(...args); } finally { accumulated += performance.now() - t0; } };
  return { take: () => { const value = accumulated; accumulated = 0; return value; } };
}

describe('fusion workload and surface quality, synthetic scans (ARCore-like noise)', () => {
  it(`${SYNTHETIC_REPEATS} repeats: sampleDepthFrame time and surface recall/precision per scene`, () => {
    const report: Record<string, unknown> = {};
    for (const [name, make] of SCENES) {
      const scene = make();
      const poses = orbitPoses([0, 0, 0.15], 0.9, 35, 0, 330, 24);
      const frames = poses.map((pose, index) => renderDepth(scene, pose, index + 1, ARCORE_NOISE, DEFAULT_INTRINSICS));   /* rendering is outside the clock */
      const all: number[] = []; const allOccupancy: number[] = []; const runTotals: number[] = [];
      let quality = { recall: 0, precision: 0, points: 0 }; let heapPeak = 0;
      for (let run = 0; run < SYNTHETIC_REPEATS + 1; run += 1) {
        const accumulator = new DepthPointCloudAccumulator();
        const occupancyClock = instrumentOccupancy(accumulator);
        const heapStart = heapMb(); const times: number[] = []; const occupancyTimes: number[] = [];
        frames.forEach((frame, index) => {
          const context: DepthFusionFrameContext = { keyframeId: index + 1, evidenceKeyframe: true, frameWeight: 1, cameraProjectPosition: poses[index].eye };
          let t0 = performance.now(); accumulator.sampleDepthFrame(frame.depth, frame.view, context); times.push(performance.now() - t0); occupancyTimes.push(occupancyClock.take());
          for (let polish = 0; polish < 2; polish += 1) {
            t0 = performance.now();
            accumulator.sampleDepthFrame(frame.depth, frame.view, { ...context, evidenceKeyframe: false, frameWeight: 0.2 });
            times.push(performance.now() - t0); occupancyTimes.push(occupancyClock.take());
          }
        });
        heapPeak = Math.max(heapPeak, heapMb() - heapStart);
        if (run === 0) continue;                                                                                          /* the first scan is the warm-up */
        all.push(...times.slice(WARMUP_CALLS)); allOccupancy.push(...occupancyTimes.slice(WARMUP_CALLS));
        runTotals.push(times.reduce((a, b) => a + b, 0));
        if (run === 1) quality = recallPrecision(scene, accumulator.snapshot(frames.length, frames.length * 3).positions);
      }
      report[name] = { callMs: summarize(all), occupancyMs: summarize(allOccupancy), scanMsMean: summarize(runTotals).mean, ...quality, heapDeltaMb: heapPeak };
      expect(quality.points, name).toBeGreaterThan(0);
    }
    writeReport('fusion', 'synthetic', report);
  }, 600_000);
});

type SequenceSource = { name: string; load: () => ReplayFrame[] | null };

const tumSource = (name: string, folder: string, camera: 'freiburg1' | 'freiburg2'): SequenceSource => ({
  name,
  load: () => { const dir = findSequenceDir(`${DATASET_ROOT}/tum/${folder}`); return dir === null ? null : loadTumLike({ dir, intrinsics: TUM_INTRINSICS[camera], depthScale: 5000, stride: 3, maxFrames: 300 }); },
});
const iclSource = (name: string, folder: string, gt: string): SequenceSource => ({
  name,
  load: () => { const dir = `${DATASET_ROOT}/icl-nuim/${folder}`; return existsSync(dir) ? loadIclSequence(dir, `${DATASET_ROOT}/icl-nuim/${gt}`, { stride: 3, maxFrames: 300 }) : null; },
});
const redwoodSource = (name: string, folder: string, traj: string): SequenceSource => ({
  name,
  load: () => { const dir = `${DATASET_ROOT}/redwood/${folder}`; return existsSync(dir) ? loadRedwoodSequence(dir, `${DATASET_ROOT}/redwood/${traj}`, { stride: 3, maxFrames: 300 }) : null; },
});

const SEQUENCES: SequenceSource[] = [
  tumSource('tum-freiburg1-xyz', 'freiburg1_xyz', 'freiburg1'),
  tumSource('tum-freiburg1-desk', 'freiburg1_desk', 'freiburg1'),
  tumSource('tum-freiburg1-room', 'freiburg1_room', 'freiburg1'),
  tumSource('tum-freiburg2-desk', 'freiburg2_desk', 'freiburg2'),
  iclSource('icl-living-kt0-clean', 'living_kt0', 'livingRoom0.gt.freiburg'),
  iclSource('icl-living-kt0-noisy', 'living_kt0_noisy', 'livingRoom0.gt.freiburg'),
  iclSource('icl-office-kt0-clean', 'office_kt0', 'traj0.gt.freiburg'),
  iclSource('icl-office-kt0-noisy', 'office_kt0_noisy', 'traj0.gt.freiburg'),
  redwoodSource('redwood-living1-clean', 'livingroom1-depth-clean', 'livingroom1-traj.txt'),
  redwoodSource('redwood-living1-noisy', 'livingroom1-depth-simulated', 'livingroom1-traj.txt'),
  redwoodSource('redwood-office1-clean', 'office1-depth-clean', 'office1-traj.txt'),
  redwoodSource('redwood-office1-noisy', 'office1-depth-simulated', 'office1-traj.txt'),
];

describe('fusion workload, public RGB-D depth/pose replay (depth resampled to 160x120, up to 300 frames)', () => {
  for (const { name, load } of SEQUENCES) {
    it(`${name}: ${DATASET_REPEATS} repeats`, () => {
      const frames = load();
      expect(frames, `dataset ${name} is not downloaded (python tools/datasets/download_3d_scanning.py download)`).not.toBeNull();
      expect(frames!.length, 'depth/pose pairs found').toBeGreaterThan(100);
      const all: number[] = []; const allOccupancy: number[] = [];
      let last = { points: 0, promoted: 0, keyframes: 0 };
      let heapPeak = 0;
      for (let run = 0; run < DATASET_REPEATS + 1; run += 1) {
        const accumulator = new DepthPointCloudAccumulator();
        const occupancyClock = instrumentOccupancy(accumulator);
        const heapStart = heapMb(); const times: number[] = []; const occupancyTimes: number[] = [];
        let keyframeId = 0; let lastKey: Float32Array | null = null;
        for (const frame of frames!) {
          const m = frame.worldFromView;
          const moved = lastKey === null ? Infinity : Math.hypot(m[12] - lastKey[12], m[13] - lastKey[13], m[14] - lastKey[14]);
          const turned = lastKey === null ? Infinity : 1 - (m[0] * lastKey[0] + m[1] * lastKey[1] + m[2] * lastKey[2] + m[4] * lastKey[4] + m[5] * lastKey[5] + m[6] * lastKey[6] + m[8] * lastKey[8] + m[9] * lastKey[9] + m[10] * lastKey[10]) / 3;
          const evidenceKeyframe = moved > 0.05 || turned > 0.004;
          if (evidenceKeyframe) { keyframeId += 1; lastKey = m; }
          const context: DepthFusionFrameContext = { keyframeId: Math.max(1, keyframeId), evidenceKeyframe, frameWeight: evidenceKeyframe ? 1 : 0.2, cameraProjectPosition: projectCameraPosition(frame) };
          const t0 = performance.now();
          accumulator.sampleDepthFrame(depthInformation(frame), { projectionMatrix: frame.projection, transform: { matrix: frame.worldFromView } }, context);
          times.push(performance.now() - t0); occupancyTimes.push(occupancyClock.take());
        }
        heapPeak = Math.max(heapPeak, heapMb() - heapStart);
        if (run === 0) continue;
        all.push(...times.slice(WARMUP_CALLS)); allOccupancy.push(...occupancyTimes.slice(WARMUP_CALLS));
        last = { points: accumulator.pointCount, promoted: accumulator.promotedCount, keyframes: accumulator.keyframeCount };
      }
      writeReport('fusion', name, { frames: frames!.length, callMs: summarize(all), occupancyMs: summarize(allOccupancy), ...last, heapDeltaMb: heapPeak });
    }, 1_800_000);
  }
});
