import { existsSync, mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, expect, it } from 'vitest';

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

const SOURCES: Array<[string, () => ReplayFrame[] | null]> = [
  ['tum-freiburg1-room', () => { const d = findSequenceDir(`${DATASET_ROOT}/tum/freiburg1_room`); return d === null ? null : loadTumLike({ dir: d, intrinsics: TUM_INTRINSICS.freiburg1, depthScale: 5000, stride: 3, maxFrames: MAX }); }],
  ['tum-freiburg2-desk', () => { const d = findSequenceDir(`${DATASET_ROOT}/tum/freiburg2_desk`); return d === null ? null : loadTumLike({ dir: d, intrinsics: TUM_INTRINSICS.freiburg2, depthScale: 5000, stride: 3, maxFrames: MAX }); }],
  ['icl-living-kt0-noisy', () => existsSync(`${DATASET_ROOT}/icl-nuim/living_kt0_noisy`) ? loadIclSequence(`${DATASET_ROOT}/icl-nuim/living_kt0_noisy`, `${DATASET_ROOT}/icl-nuim/livingRoom0.gt.freiburg`, { stride: 3, maxFrames: MAX }) : null],
  ['redwood-living1-noisy', () => existsSync(`${DATASET_ROOT}/redwood/livingroom1-depth-simulated`) ? loadRedwoodSequence(`${DATASET_ROOT}/redwood/livingroom1-depth-simulated`, `${DATASET_ROOT}/redwood/livingroom1-traj.txt`, { stride: 3, maxFrames: MAX }) : null],
  ['redwood-office1-noisy', () => existsSync(`${DATASET_ROOT}/redwood/office1-depth-simulated`) ? loadRedwoodSequence(`${DATASET_ROOT}/redwood/office1-depth-simulated`, `${DATASET_ROOT}/redwood/office1-traj.txt`, { stride: 3, maxFrames: MAX }) : null],
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
