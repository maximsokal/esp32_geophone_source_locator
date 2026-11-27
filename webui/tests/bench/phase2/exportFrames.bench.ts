import { existsSync, mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, expect, it } from 'vitest';

import { ARCORE_NOISE, boxCornerScene, DEFAULT_INTRINSICS, insideCornerScene, orbitPoses, renderDepth, stepScene, tiltedPlaneScene, type SyntheticScene, type Vec3 } from '../../xr/support/syntheticDepthScene';
import { DATASET_ROOT, findSequenceDir, loadIclSequence, loadRedwoodSequence, loadTumLike, projectCameraPosition, TUM_INTRINSICS, type ReplayFrame } from './support/replay';

/*
 * Writes depth/pose replays as compact packs for the browser benchmark (scripts/bench-fusion-worker.mjs), which needs REAL Workers and therefore runs in
 * Chromium, not Node. Pack: [u32 headerBytes][header JSON][frames...], each frame = u16 depth in millimetres (0 = missing) | f32 worldFromView[16] |
 * f32 cameraProjectPosition[3] | u32 keyframeId | u32 keyframeFlag. The packs stay under .test-build (private/derived data, never committed).
 */
const OUT = resolve(__dirname, '../../../../.test-build/phase2/packs');
const MAX = Number(process.env.PHASE2_PACK_FRAMES ?? 300);

function keyframes(frames: ReplayFrame[]): Array<{ keyframeId: number; keyframe: boolean }> {
  let keyframeId = 0; let lastKey: Float32Array | null = null;
  return frames.map((frame) => {
    const m = frame.worldFromView;
    const moved = lastKey === null ? Infinity : Math.hypot(m[12] - lastKey[12], m[13] - lastKey[13], m[14] - lastKey[14]);
    const turned = lastKey === null ? Infinity : 1 - (m[0] * lastKey[0] + m[1] * lastKey[1] + m[2] * lastKey[2] + m[4] * lastKey[4] + m[5] * lastKey[5] + m[6] * lastKey[6] + m[8] * lastKey[8] + m[9] * lastKey[9] + m[10] * lastKey[10]) / 3;
    const keyframe = moved > 0.05 || turned > 0.004;
    if (keyframe) { keyframeId += 1; lastKey = m; }
    return { keyframeId: Math.max(1, keyframeId), keyframe };
  });
}

function writePack(name: string, frames: ReplayFrame[]): void {
  mkdirSync(OUT, { recursive: true });
  const width = frames[0].width; const height = frames[0].height;
  const header = Buffer.from(JSON.stringify({ name, frames: frames.length, width, height, projection: Array.from(frames[0].projection) }));
  const flags = keyframes(frames);
  const frameBytes = width * height * 2 + 16 * 4 + 3 * 4 + 4 + 4;
  const body = Buffer.alloc(frames.length * frameBytes);
  let offset = 0;
  frames.forEach((frame, index) => {
    for (let i = 0; i < width * height; i += 1) { const meters = frame.depth[i]; body.writeUInt16LE(Number.isFinite(meters) ? Math.min(65535, Math.round(meters * 1000)) : 0, offset + i * 2); }
    offset += width * height * 2;
    for (let i = 0; i < 16; i += 1) { body.writeFloatLE(frame.worldFromView[i], offset); offset += 4; }
    const eye = projectCameraPosition(frame);
    for (let i = 0; i < 3; i += 1) { body.writeFloatLE(eye[i], offset); offset += 4; }
    body.writeUInt32LE(flags[index].keyframeId, offset); offset += 4;
    body.writeUInt32LE(flags[index].keyframe ? 1 : 0, offset); offset += 4;
  });
  const prefix = Buffer.alloc(4); prefix.writeUInt32LE(header.length, 0);
  writeFileSync(resolve(OUT, `${name}.pack`), Buffer.concat([prefix, header, body]));
}

const tum = (name: string, intrinsics: (typeof TUM_INTRINSICS)['freiburg1']): ReplayFrame[] | null => { const dir = findSequenceDir(`${DATASET_ROOT}/tum/${name}`); return dir === null ? null : loadTumLike({ dir, intrinsics, depthScale: 5000, stride: 3, maxFrames: MAX }); };
const icl = (folder: string, truth: string): ReplayFrame[] | null => (existsSync(`${DATASET_ROOT}/icl-nuim/${folder}`) ? loadIclSequence(`${DATASET_ROOT}/icl-nuim/${folder}`, `${DATASET_ROOT}/icl-nuim/${truth}.gt.freiburg`, { stride: 3, maxFrames: MAX }) : null);
const redwood = (folder: string, truth: string): ReplayFrame[] | null => (existsSync(`${DATASET_ROOT}/redwood/${folder}`) ? loadRedwoodSequence(`${DATASET_ROOT}/redwood/${folder}`, `${DATASET_ROOT}/redwood/${truth}-traj.txt`, { stride: 3, maxFrames: MAX }) : null);

const synthetic = (scene: () => SyntheticScene, target: Vec3, radius: number, elevation: number, from: number, to: number): ReplayFrame[] =>
  orbitPoses(target, radius, elevation, from, to, 90).map((pose, index) => {
    const rendered = renderDepth(scene(), pose, index + 1, ARCORE_NOISE, DEFAULT_INTRINSICS);
    return { timestamp: index / 10, width: DEFAULT_INTRINSICS.width, height: DEFAULT_INTRINSICS.height, depth: rendered.buffer, projection: Float32Array.from(rendered.view.projectionMatrix), worldFromView: Float32Array.from(rendered.view.transform.matrix) };
  });

const SOURCES: Array<[string, () => ReplayFrame[] | null]> = [
  ['synthetic-inside-corner', () => synthetic(() => insideCornerScene([0, 0, 0], 0.6), [0, 0, 0.15], 0.9, 35, 0, 330)],
  ['synthetic-box-corner', () => synthetic(() => boxCornerScene([0, 0, 0], 0.4), [0, 0, 0], 0.9, 35, -20, 120)],
  ['synthetic-step', () => synthetic(() => stepScene(0.2, 0.6), [0, 0, 0.1], 1.0, 45, 0, 330)],
  ['synthetic-tilted30', () => synthetic(() => tiltedPlaneScene(30, 20, 0.8, [0, 0, 0]), [0, 0, 0], 1.0, 50, 0, 330)],
  ['tum-freiburg1-xyz', () => tum('freiburg1_xyz', TUM_INTRINSICS.freiburg1)],
  ['tum-freiburg1-desk', () => tum('freiburg1_desk', TUM_INTRINSICS.freiburg1)],
  ['tum-freiburg1-room', () => tum('freiburg1_room', TUM_INTRINSICS.freiburg1)],
  ['tum-freiburg2-desk', () => tum('freiburg2_desk', TUM_INTRINSICS.freiburg2)],
  ...([['living_kt0', 'livingRoom0'], ['living_kt0_noisy', 'livingRoom0'], ['living_kt1', 'livingRoom1'], ['living_kt1_noisy', 'livingRoom1'], ['office_kt0', 'traj0'], ['office_kt0_noisy', 'traj0'], ['office_kt1', 'traj1'], ['office_kt1_noisy', 'traj1']] as const).map(([folder, truth]): [string, () => ReplayFrame[] | null] => [`icl-${folder.replace(/_/g, '-')}`, () => icl(folder, truth)]),
  ...([['redwood-living1-clean', 'livingroom1-depth-clean', 'livingroom1'], ['redwood-living1-noisy', 'livingroom1-depth-simulated', 'livingroom1'], ['redwood-living2-clean', 'livingroom2-depth-clean', 'livingroom2'], ['redwood-living2-noisy', 'livingroom2-depth-simulated', 'livingroom2'], ['redwood-office1-clean', 'office1-depth-clean', 'office1'], ['redwood-office1-noisy', 'office1-depth-simulated', 'office1'], ['redwood-office2-clean', 'office2-depth-clean', 'office2'], ['redwood-office2-noisy', 'office2-depth-simulated', 'office2']] as const).map(([name, folder, truth]): [string, () => ReplayFrame[] | null] => [name, () => redwood(folder, truth)]),
];

describe('depth/pose packs for the browser benchmark', () => {
  for (const [name, load] of SOURCES) {
    it(name, () => {
      const frames = load();
      expect(frames, `dataset for ${name} is not downloaded`).not.toBeNull();
      writePack(name, frames!);
    }, 600_000);
  }
});
