import { readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, expect, it } from 'vitest';

import { DepthPointCloudAccumulator } from '../../../src/xr/depthPointCloud';

/*
 * WHY does reconstruction recall fall when the fusion is slow, although no accepted frame is lost? The XR replay (scripts/bench-xr-replay.mjs) records every
 * depth frame the runtime captured. Here those recorded sets are fed to the same accumulator again, first unchanged (which must reproduce the live result),
 * then with frame classes taken out or put back, one class at a time.
 *   PHASE2_RUN_FAST / PHASE2_RUN_SLOW: replay reports (JSON) of the unslowed and the slowed runs; PHASE2_PACK: the pack they replayed.
 */
const ROOT = resolve(__dirname, '../../../../.test-build/phase2');
const FAST = process.env.PHASE2_RUN_FAST ?? `${ROOT}/xr-replay-sweep-head-0.json`;
const SLOW = process.env.PHASE2_RUN_SLOW ?? `${ROOT}/xr-replay-sweep-head-600.json`;
const PACK = process.env.PHASE2_PACK ?? `${ROOT}/packs/redwood-living1-noisy.pack`;
const OUT = process.env.PHASE2_OUT ?? `${ROOT}/recall-decomposition.json`;

interface Pack { width: number; height: number; projection: Float32Array; frames: Array<{ depth: ArrayBuffer; worldFromView: Float32Array; eye: [number, number, number]; keyframeId: number; keyframe: boolean }> }
type Recorded = [index: number, evidenceKeyframe: number, frameWeight: number, keyframeId: number, frameClass: string];
interface Capture { step: number; index: number; evidence: boolean; weight: number; keyframeId: number }

function parsePack(buffer: ArrayBuffer): Pack {
  const view = new DataView(buffer);
  const headerBytes = view.getUint32(0, true);
  const header = JSON.parse(new TextDecoder().decode(new Uint8Array(buffer, 4, headerBytes))) as { frames: number; width: number; height: number; projection: number[] };
  const depthBytes = header.width * header.height * 2;
  const frameBytes = depthBytes + 16 * 4 + 3 * 4 + 4 + 4;
  const frames: Pack['frames'] = [];
  for (let i = 0; i < header.frames; i += 1) {
    const base = 4 + headerBytes + i * frameBytes;
    const matrix = new Float32Array(16); for (let k = 0; k < 16; k += 1) matrix[k] = view.getFloat32(base + depthBytes + k * 4, true);
    const eyeBase = base + depthBytes + 64;
    frames.push({
      depth: buffer.slice(base, base + depthBytes), worldFromView: matrix,
      eye: [view.getFloat32(eyeBase, true), view.getFloat32(eyeBase + 4, true), view.getFloat32(eyeBase + 8, true)],
      keyframeId: view.getUint32(eyeBase + 12, true), keyframe: view.getUint32(eyeBase + 16, true) === 1,
    });
  }
  return { width: header.width, height: header.height, projection: Float32Array.from(header.projection), frames };
}

const IDENTITY = Float32Array.from([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);

function integrate(pack: Pack, accumulator: DepthPointCloudAccumulator, index: number, context: { keyframeId: number; evidenceKeyframe: boolean; frameWeight: number }): void {
  const { width, height } = pack; const frame = pack.frames[index];
  const values = new Uint16Array(frame.depth);
  const depth = {
    width, height, data: frame.depth, rawValueToMeters: 0.001, normDepthBufferFromNormView: { matrix: IDENTITY },
    projectionMatrix: pack.projection, transform: { matrix: frame.worldFromView },
    getDepthInMeters(x: number, y: number): number { const v = values[Math.min(height - 1, Math.trunc(y * height)) * width + Math.min(width - 1, Math.trunc(x * width))]; return v === 0 ? Number.NaN : v * 0.001; },
  };
  accumulator.sampleDepthFrame(depth as never, { projectionMatrix: pack.projection, transform: { matrix: frame.worldFromView } }, { ...context, cameraProjectPosition: frame.eye });
}

/** The replay plays the pack forwards then backwards; `step` is the moment on that timeline, so two recorded sets can be merged in time order. */
function timeline(recorded: Recorded[], length: number): Capture[] {
  let backwards = false; let previous = -1;
  return recorded.map(([index, evidence, weight, keyframeId]) => {
    if (index < previous - 2) backwards = true;
    previous = index;
    return { step: backwards ? 2 * (length - 1) - index : index, index, evidence: evidence === 1, weight, keyframeId };
  });
}

/** Keyframe ids must be one per evidence keyframe, in order; a non-keyframe belongs to the keyframe before it. */
function renumber(captures: Capture[]): Capture[] {
  let id = 0;
  return captures.map((capture) => { if (capture.evidence) id += 1; return { ...capture, keyframeId: Math.max(1, id) }; });
}

/** `base` plus those of `extra` (chosen by `take`) taken at a moment `base` captured nothing. */
function merge(base: Capture[], extra: Capture[], take: (capture: Capture) => boolean): Capture[] {
  const taken = new Set(base.map((capture) => capture.step));
  return renumber([...base, ...extra.filter((capture) => take(capture) && !taken.has(capture.step))].sort((a, b) => a.step - b.step));
}

const TOLERANCE = 0.03;
function share(reference: Float32Array, got: Float32Array): number {
  const grid = new Map<string, number[]>();
  for (let i = 0; i + 2 < got.length; i += 3) { const key = `${Math.floor(got[i] / TOLERANCE)},${Math.floor(got[i + 1] / TOLERANCE)},${Math.floor(got[i + 2] / TOLERANCE)}`; const list = grid.get(key); if (list) list.push(i); else grid.set(key, [i]); }
  let hits = 0;
  for (let i = 0; i + 2 < reference.length; i += 3) {
    const cx = Math.floor(reference[i] / TOLERANCE); const cy = Math.floor(reference[i + 1] / TOLERANCE); const cz = Math.floor(reference[i + 2] / TOLERANCE);
    search: for (let dx = -1; dx <= 1; dx += 1) for (let dy = -1; dy <= 1; dy += 1) for (let dz = -1; dz <= 1; dz += 1) for (const j of grid.get(`${cx + dx},${cy + dy},${cz + dz}`) ?? []) {
      if (Math.hypot(reference[i] - got[j], reference[i + 1] - got[j + 1], reference[i + 2] - got[j + 2]) <= TOLERANCE) { hits += 1; break search; }
    }
  }
  return reference.length === 0 ? 0 : hits / (reference.length / 3);
}

interface Measured { frames: number; keyframes: number; points: number; confirmed: number; observed: number }

describe('why recall falls when the fusion is slow', () => {
  it('decomposes the loss into never observed and observed but not confirmed, and into the frame classes that were not captured', () => {
    const raw = readFileSync(PACK);
    const pack = parsePack(raw.buffer.slice(raw.byteOffset, raw.byteOffset + raw.byteLength) as ArrayBuffer);
    const reference = new DepthPointCloudAccumulator();
    pack.frames.forEach((frame, i) => integrate(pack, reference, i, { keyframeId: frame.keyframeId, evidenceKeyframe: frame.keyframe, frameWeight: frame.keyframe ? 1 : 0.2 }));
    const full = reference.snapshot(pack.frames.length, pack.frames.length);

    const measure = (captures: Capture[]): Measured => {
      const accumulator = new DepthPointCloudAccumulator();
      for (const capture of captures) integrate(pack, accumulator, capture.index, { keyframeId: capture.keyframeId, evidenceKeyframe: capture.evidence, frameWeight: capture.weight });
      const snapshot = accumulator.snapshot(captures.length, captures.length);
      const seen = new Float32Array(snapshot.positions.length + snapshot.tentativePositions.length); seen.set(snapshot.positions, 0); seen.set(snapshot.tentativePositions, snapshot.positions.length);
      return { frames: captures.length, keyframes: captures.filter((c) => c.evidence).length, points: snapshot.pointCount, confirmed: share(full.positions, snapshot.positions), observed: share(full.positions, seen) };
    };

    type Run = { captured: Recorded[]; finalPoints: number };
    const load = (path: string): Run[] => (JSON.parse(readFileSync(path, 'utf8')) as { runs: Run[] }).runs;
    const fastRuns = load(FAST); const slowRuns = load(SLOW);
    const pairs = Math.min(fastRuns.length, slowRuns.length);
    const variants: Record<string, Measured[]> = {};
    const add = (name: string, value: Measured): void => { (variants[name] ??= []).push(value); };
    for (let i = 0; i < pairs; i += 1) {
      const fast = timeline(fastRuns[i].captured, pack.frames.length); const slow = timeline(slowRuns[i].captured, pack.frames.length);
      const fastAsRecorded = measure(fast); const slowAsRecorded = measure(slow);
      expect(fastAsRecorded.points, 'replaying the recorded frames reproduces the live scan').toBe(fastRuns[i].finalPoints);
      expect(slowAsRecorded.points, 'replaying the recorded frames reproduces the live scan').toBe(slowRuns[i].finalPoints);
      add('A fast, as recorded', fastAsRecorded);
      add('B slow, as recorded', slowAsRecorded);
      add('C slow + the NON-keyframes only the fast run had', measure(merge(slow, fast, (c) => !c.evidence)));
      add('D slow + the KEYFRAMES only the fast run had', measure(merge(slow, fast, (c) => c.evidence)));
      add('E fast without any non-keyframe', measure(renumber(fast.filter((c) => c.evidence))));
      add('F slow without any non-keyframe', measure(renumber(slow.filter((c) => c.evidence))));
      let keyframe = 0;
      add('G fast with every second keyframe removed', measure(renumber(fast.filter((c) => !c.evidence || (keyframe += 1) % 2 === 1))));
    }
    const mean = (values: number[]): number => values.reduce((a, b) => a + b, 0) / values.length;
    const spread = (values: number[]): number => Math.sqrt(mean(values.map((v) => (v - mean(values)) ** 2)));
    const table = Object.entries(variants).map(([name, list]) => ({
      variant: name, frames: Math.round(mean(list.map((m) => m.frames))), keyframes: Math.round(mean(list.map((m) => m.keyframes))), points: Math.round(mean(list.map((m) => m.points))),
      confirmed: mean(list.map((m) => m.confirmed)), confirmedSd: spread(list.map((m) => m.confirmed)), observed: mean(list.map((m) => m.observed)),
      confirmedOfObserved: mean(list.map((m) => m.confirmed / m.observed)),
    }));
    writeFileSync(OUT, JSON.stringify({ pairs, referencePoints: full.pointCount, table }, null, 2));
    console.log(table.map((r) => `${r.variant.padEnd(52)} frames ${String(r.frames).padStart(3)} kf ${String(r.keyframes).padStart(3)} pts ${r.points}  observed ${r.observed.toFixed(3)}  confirmed ${r.confirmed.toFixed(3)} ±${r.confirmedSd.toFixed(3)}  confirmed/observed ${r.confirmedOfObserved.toFixed(3)}`).join('\n'));
  }, 600_000);
});
