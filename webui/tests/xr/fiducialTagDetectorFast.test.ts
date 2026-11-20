import { describe, expect, it, vi } from 'vitest';

const luma = vi.hoisted(() => ({ calls: 0 }));
vi.mock('../../src/xr/fiducial/cornerRefine', async (importOriginal) => {
  const original = await importOriginal<typeof import('../../src/xr/fiducial/cornerRefine')>();
  return { ...original, grayscaleLuma: (...args: Parameters<typeof original.grayscaleLuma>) => { luma.calls += 1; return original.grayscaleLuma(...args); } };
});

import {
  chooseDetectionPass, detectAprilTags, detectAprilTagsCoarse, detectAprilTagsFast, detectAprilTagsPass, detectAprilTagsPassDetailed, TAG_FULL_PASS_EVERY, type DetectedAprilTag,
} from '../../src/xr/fiducial/tagDetector';
import { blankFrame, rasterTag } from '../tag-lab/support/raster';

const W = 1280; const H = 720;
const tag = (id: number, options: { left?: number; top?: number; cell?: number } = {}) => rasterTag(id, { width: W, height: H, left: 500, top: 250, cell: 16, ...options });
const px = (d: DetectedAprilTag) => d.corners.map(([x, y]) => [x * (W - 1), y * (H - 1)] as const);
const dist = (a: readonly number[], b: readonly number[]) => Math.hypot(a[0] - b[0], a[1] - b[1]);

/** Several tags stamped onto one white frame (black wins). */
function sheet(placements: { id: number; left: number; top: number; cell?: number }[]): Uint8Array {
  const frame = blankFrame(W, H);
  for (const { id, left, top, cell } of placements) {
    const layer = tag(id, { left, top, cell: cell ?? 16 }).rgba;
    for (let i = 0; i < frame.length; i += 4) if (layer[i] < 128) { frame[i] = 0; frame[i + 1] = 0; frame[i + 2] = 0; }
  }
  return frame;
}

function degrade(rgba: Uint8Array, noise: number, blurPasses: number): Uint8Array {
  const out = rgba.slice();
  for (let pass = 0; pass < blurPasses; pass += 1) {
    const copy = out.slice();
    for (let y = 1; y < H - 1; y += 1) for (let x = 1; x < W - 1; x += 1) {
      let sum = 0;
      for (let dy = -1; dy <= 1; dy += 1) for (let dx = -1; dx <= 1; dx += 1) sum += copy[((y + dy) * W + x + dx) * 4];
      const o = (y * W + x) * 4; const v = sum / 9; out[o] = v; out[o + 1] = v; out[o + 2] = v;
    }
  }
  let seed = 4242;
  for (let i = 0; i < out.length; i += 4) {
    seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
    const v = Math.max(0, Math.min(255, out[i] + ((seed >>> 8) / 0xffffff - 0.5) * 2 * noise));
    out[i] = v; out[i + 1] = v; out[i + 2] = v;
  }
  return out;
}

describe('fast detector pass (half-size decode, full-resolution corner refinement)', () => {
  it.each([0, 1, 2, 3, 4])('finds tag %i with the same id and corners as the full pass (within 1 px)', (id) => {
    const frame = tag(id).rgba;
    const full = detectAprilTags(W, H, frame);
    const fast = detectAprilTagsFast(W, H, frame);
    expect(full.map((d) => d.tagId)).toEqual([id]);
    expect(fast.map((d) => d.tagId)).toEqual([id]);
    expect(fast[0].hammingDistance).toBe(0);
    px(fast[0]).forEach((corner, i) => expect(dist(corner, px(full[0])[i])).toBeLessThan(1));
  });

  it('keeps tag-plane corner order and lands on the drawn tag (BL, BR, TR, TL)', () => {
    const r = tag(2);
    const [d] = detectAprilTagsFast(W, H, r.rgba);
    const truth = [[r.left, r.top + r.tagSize], [r.left + r.tagSize, r.top + r.tagSize], [r.left + r.tagSize, r.top], [r.left, r.top]];
    px(d).forEach((corner, i) => expect(dist(corner, truth[i])).toBeLessThan(2.5));
  });

  it('stays within 1.5 px of the full pass on a blurred, noisy frame', () => {
    const frame = degrade(tag(3).rgba, 20, 1);
    const full = detectAprilTags(W, H, frame); const fast = detectAprilTagsFast(W, H, frame);
    expect(full.map((d) => d.tagId)).toEqual([3]);
    expect(fast.map((d) => d.tagId)).toEqual([3]);
    px(fast[0]).forEach((corner, i) => expect(dist(corner, px(full[0])[i])).toBeLessThan(1.5));
  });

  it('reaches down to about a 24 px tag at half size; a 16 px tag is only found by the periodic full pass', () => {
    const edge24 = tag(1, { cell: 3 }).rgba;
    expect(detectAprilTagsFast(W, H, edge24).map((d) => d.tagId)).toEqual([1]);
    const edge16 = tag(1, { cell: 2 }).rgba; // measured limit: decodable at full size, below the half-size reach
    expect(detectAprilTags(W, H, edge16).map((d) => d.tagId)).toEqual([1]);
    expect(detectAprilTagsFast(W, H, edge16)).toEqual([]);
  });

  it('is at least as robust as the full pass on a noisy, blurred 32 px tag (the 2x2 averaging suppresses noise)', () => {
    const noisy = degrade(tag(1, { cell: 4 }).rgba, 30, 2);
    expect(detectAprilTagsFast(W, H, noisy).map((d) => d.tagId)).toEqual([1]);
    expect(detectAprilTags(W, H, noisy).length).toBeLessThanOrEqual(1);
  });

  /** A dim phone frame: brightness scaled down, offset, strong sensor noise (different noise per seed). */
  const dim = (rgba: Uint8Array, gain: number, offset: number, noise: number, seed0: number): Uint8Array => {
    const out = rgba.slice(); let seed = seed0;
    for (let i = 0; i < out.length; i += 4) {
      seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
      const v = Math.max(0, Math.min(255, out[i] * gain + offset + ((seed >>> 8) / 0xffffff - 0.5) * 2 * noise));
      out[i] = v; out[i + 1] = v; out[i + 2] = v;
    }
    return out;
  };
  const hits = (detect: (frame: Uint8Array) => DetectedAprilTag[], frames: Uint8Array[], id: number) => frames.filter((frame) => detect(frame).some((d) => d.tagId === id)).length;

  it('rescues very dark, noisy frames with the coarse pass, which the half-size and full passes lose', () => {
    const frames = [1, 2, 3, 4, 5, 6].map((k) => dim(tag(0, { left: 480 + k * 5, top: 240 + k * 3, cell: 8 }).rgba, 0.2, 15, 30, 70 + k));
    const full = hits((f) => detectAprilTags(W, H, f), frames, 0);
    const fast = hits((f) => detectAprilTagsFast(W, H, f), frames, 0);
    const coarse = hits((f) => detectAprilTagsCoarse(W, H, f), frames, 0);
    expect(full).toBeLessThanOrEqual(1);
    expect(fast).toBeLessThanOrEqual(2);
    expect(coarse).toBeGreaterThanOrEqual(3);
    expect(coarse).toBeGreaterThan(fast);
    // The scheduled 'fast' pass falls back to it, so the worker needs no extra round trip.
    const used = frames.map((f) => detectAprilTagsPassDetailed('fast', W, H, f)).filter((r) => r.detections.some((d) => d.tagId === 0));
    expect(used.length).toBeGreaterThanOrEqual(3);
    expect(used.some((r) => r.used === 'coarse')).toBe(true);
  });

  it('reports which pass produced the result and does not fall back when the fast pass already sees the tag', () => {
    const clean = tag(3).rgba;
    expect(detectAprilTagsPassDetailed('full', W, H, clean)).toMatchObject({ used: 'full', detections: [{ tagId: 3 }] });
    expect(detectAprilTagsPassDetailed('fast', W, H, clean)).toMatchObject({ used: 'fast', detections: [{ tagId: 3 }] });
    expect(detectAprilTagsPassDetailed('fast', W, H, blankFrame(W, H))).toEqual({ detections: [], used: 'fast' });
  });

  it('keeps coarse-pass corners within 1.5 px of the full pass on a clean frame (refined at full resolution)', () => {
    const frame = tag(4).rgba;
    const full = detectAprilTags(W, H, frame); const coarse = detectAprilTagsCoarse(W, H, frame);
    expect(coarse.map((d) => d.tagId)).toEqual([4]);
    px(coarse[0]).forEach((corner, i) => expect(dist(corner, px(full[0])[i])).toBeLessThan(1.5));
  });

  it('returns nothing for a blank or undersized frame from the coarse pass', () => {
    expect(detectAprilTagsCoarse(W, H, blankFrame(W, H))).toEqual([]);
    expect(detectAprilTagsCoarse(16, 16, new Uint8Array(16 * 16 * 4))).toEqual([]);
    expect(detectAprilTagsCoarse(W, H, new Uint8Array(100))).toEqual([]);
  });

  it('chooses a full pass for the first frame and every Nth, fast otherwise', () => {
    expect(TAG_FULL_PASS_EVERY).toBe(4);
    expect([0, 1, 2, 3, 4, 5, 8, 9].map((i) => chooseDetectionPass(i))).toEqual(['full', 'fast', 'fast', 'fast', 'full', 'fast', 'full', 'fast']);
    expect([0, 1, 2, 3].map((i) => chooseDetectionPass(i, 2))).toEqual(['full', 'fast', 'full', 'fast']);
    const frame = tag(4).rgba;
    expect(detectAprilTagsPass('full', W, H, frame)[0].tagId).toBe(4);
    expect(detectAprilTagsPass('fast', W, H, frame)[0].tagId).toBe(4);
  });

  it('returns nothing for a blank frame, a tiny frame or a short buffer, and is not stateful across calls', () => {
    expect(detectAprilTagsFast(W, H, blankFrame(W, H))).toEqual([]);
    expect(detectAprilTagsFast(8, 8, new Uint8Array(8 * 8 * 4))).toEqual([]);
    expect(detectAprilTagsFast(W, H, new Uint8Array(100))).toEqual([]);
    const a = tag(0).rgba; const b = tag(4, { left: 300, top: 100 }).rgba;
    const first = detectAprilTagsFast(W, H, a).map((d) => d.tagId);
    expect(detectAprilTagsFast(W, H, b).map((d) => d.tagId)).toEqual([4]);
    expect(detectAprilTagsFast(W, H, a).map((d) => d.tagId)).toEqual(first);
  });

  it('computes the luma plane once per frame however many tags it refines', () => {
    const frame = sheet([{ id: 0, left: 60, top: 60 }, { id: 1, left: 330, top: 60 }, { id: 2, left: 600, top: 60 }, { id: 3, left: 870, top: 60 }, { id: 4, left: 60, top: 400 }]);
    luma.calls = 0;
    expect(detectAprilTags(W, H, frame).map((d) => d.tagId).sort()).toEqual([0, 1, 2, 3, 4]);
    expect(luma.calls).toBe(1);
    luma.calls = 0;
    expect(detectAprilTagsFast(W, H, frame).map((d) => d.tagId).sort()).toEqual([0, 1, 2, 3, 4]);
    expect(luma.calls).toBe(1);
  });

  it('is clearly cheaper than the full pass on a 1280x720 frame', () => {
    const frame = degrade(sheet([{ id: 0, left: 200, top: 200 }, { id: 3, left: 800, top: 300 }]), 6, 0);
    const median = (fn: () => unknown) => {
      fn(); fn(); // warm-up
      const times: number[] = [];
      for (let i = 0; i < 7; i += 1) { const t = performance.now(); fn(); times.push(performance.now() - t); }
      return times.sort((a, b) => a - b)[3];
    };
    const full = median(() => detectAprilTags(W, H, frame));
    const fast = median(() => detectAprilTagsFast(W, H, frame));
    // Per-frame JS overhead dominates at this size, so the ratio is modest and noisy; a regression to full cost would put it at ~1.
    expect(fast).toBeLessThan(full * 0.9);
  });
});
