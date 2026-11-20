import { grayscaleLuma, refineTagCornersLuma, type PixelQuad } from './cornerRefine';
import { AR, type ArucoMarker } from './jsAruco2';
import type { NormalizedTagCorner } from './tagPose';

// js-aruco2 compares distance < tau, so 2 permits at most one corrected bit;
// AprilTag 36h11 can correct more, but one bit is the conservative false-ID limit here.
export const TAG_DETECTOR_MAX_HAMMING_DISTANCE = 2;
export const TAG_DETECTOR_MIN_EDGE_PX = 12;

export interface DetectedAprilTag {
  tagId: 0 | 1 | 2 | 3 | 4;
  /** tagPose order: bottom-left, bottom-right, top-right, top-left; v increases down. */
  corners: readonly [NormalizedTagCorner, NormalizedTagCorner, NormalizedTagCorner, NormalizedTagCorner];
  hammingDistance: number;
}

export function mapArucoCornersToTagPose(corners: PixelQuad): PixelQuad {
  // js-aruco2 returns code-upright clockwise image corners (top-left first).
  // Reverse the cycle to tagPose's counter-clockwise bottom-left-first contract.
  return [corners[3], corners[2], corners[1], corners[0]];
}

function pixelCorners(marker: ArucoMarker): PixelQuad {
  return marker.corners.slice(0, 4).map((point) => [point.x, point.y] as const) as unknown as PixelQuad;
}

function edgeSize(corners: PixelQuad): number {
  return Math.min(...corners.map((point, index) => Math.hypot(
    point[0] - corners[(index + 1) % 4][0], point[1] - corners[(index + 1) % 4][1],
  )));
}

/* The dictionary tables are built once; a detector instance keeps only reusable scratch images between frames. */
let sharedDetector: InstanceType<typeof AR.Detector> | null = null;
function detector(): InstanceType<typeof AR.Detector> {
  sharedDetector ??= new AR.Detector({ dictionaryName: 'APRILTAG_36h11', maxHammingDistance: TAG_DETECTOR_MAX_HAMMING_DISTANCE });
  return sharedDetector;
}

/** `toFull` maps a detector-image point into full-frame pixel coordinates (identity for a full-resolution pass). */
function collectDetections(
  markers: ArucoMarker[], toFull: (x: number, y: number) => readonly [number, number], minEdgePx: number,
  width: number, height: number, rgba: Uint8Array,
): DetectedAprilTag[] {
  const result: DetectedAprilTag[] = [];
  let luma: Float32Array | null = null;
  for (const marker of markers) {
    if (marker.id < 0 || marker.id > 4 || marker.corners.length !== 4) continue;
    const found = pixelCorners(marker);
    if (edgeSize(found) < minEdgePx) continue;
    const raw = found.map(([x, y]) => toFull(x, y)) as unknown as PixelQuad;
    luma ??= width >= 3 && height >= 3 ? grayscaleLuma(rgba, width, height) : null; // once per frame, not once per tag
    const refined = luma === null ? raw : refineTagCornersLuma(raw, luma, width, height);
    const ordered = mapArucoCornersToTagPose(refined);
    result.push({
      tagId: marker.id as 0 | 1 | 2 | 3 | 4,
      corners: ordered.map(([x, y]) => [x / (width - 1), y / (height - 1)] as const) as unknown as DetectedAprilTag['corners'],
      hammingDistance: marker.hammingDistance,
    });
  }
  return onePerTagId(result);
}

/** Full-resolution pass: the reference behaviour, finds the smallest tags (>= TAG_DETECTOR_MIN_EDGE_PX). */
export function detectAprilTags(width: number, height: number, rgba: Uint8Array): DetectedAprilTag[] {
  if (width <= 0 || height <= 0 || rgba.length < width * height * 4) return [];
  const markers = detector().detectImage(width, height, rgba);
  return collectDetections(markers, (x, y) => [x, y], TAG_DETECTOR_MIN_EDGE_PX, width, height, rgba);
}

/** RGBA reduced by an integer factor (2 or 4) with box averaging; a low-image pixel index i covers full pixels f*i .. f*i+f-1 (centre f*i + (f-1)/2). */
function downscale(width: number, height: number, rgba: Uint8Array, factor: 2 | 4): { width: number; height: number; rgba: Uint8Array } {
  const w = Math.floor(width / factor); const h = Math.floor(height / factor);
  const out = new Uint8Array(w * h * 4);
  const shift = factor === 2 ? 2 : 4; // log2(factor * factor)
  if (factor === 2) { // the hot path of every frame: unrolled
    for (let y = 0; y < h; y += 1) {
      const top = (2 * y) * width * 4; const bottom = top + width * 4;
      for (let x = 0; x < w; x += 1) {
        const a = top + 8 * x; const b = bottom + 8 * x; const o = (y * w + x) * 4;
        out[o] = (rgba[a] + rgba[a + 4] + rgba[b] + rgba[b + 4]) >> 2;
        out[o + 1] = (rgba[a + 1] + rgba[a + 5] + rgba[b + 1] + rgba[b + 5]) >> 2;
        out[o + 2] = (rgba[a + 2] + rgba[a + 6] + rgba[b + 2] + rgba[b + 6]) >> 2;
        out[o + 3] = 255;
      }
    }
    return { width: w, height: h, rgba: out };
  }
  for (let y = 0; y < h; y += 1) {
    for (let x = 0; x < w; x += 1) {
      let r = 0; let g = 0; let b = 0;
      for (let dy = 0; dy < factor; dy += 1) {
        const row = ((y * factor + dy) * width + x * factor) * 4;
        for (let dx = 0; dx < factor; dx += 1) { const o = row + dx * 4; r += rgba[o]; g += rgba[o + 1]; b += rgba[o + 2]; }
      }
      const o = (y * w + x) * 4;
      out[o] = r >> shift; out[o + 1] = g >> shift; out[o + 2] = b >> shift; out[o + 3] = 255;
    }
  }
  return { width: w, height: h, rgba: out };
}

/**
 * Fast pass: decode on a half-size image (about a quarter of the work), then refine the corners on the FULL-resolution frame,
 * so accuracy stays subpixel. Averaging 2x2 also halves the image noise: on a dim, noisy frame it finds tags the full-resolution
 * pass cannot (measured: brightness x0.35, noise +-14: full 0/6, fast 6/6). Tags with a full-resolution edge under about
 * 2 x TAG_DETECTOR_MIN_EDGE_PX are not decodable at half size; the periodic full pass covers those.
 */
export function detectAprilTagsFast(width: number, height: number, rgba: Uint8Array): DetectedAprilTag[] {
  if (width < 16 || height < 16 || rgba.length < width * height * 4) return [];
  const low = downscale(width, height, rgba, 2);
  const markers = detector().detectImage(low.width, low.height, low.rgba);
  return collectDetections(markers, (x, y) => [2 * x + 0.5, 2 * y + 0.5], TAG_DETECTOR_MIN_EDGE_PX / 2, width, height, rgba);
}

/**
 * Coarse pass: a quarter-size image, 1/16 of the work. The noise is cut a further 2x, which rescues very dark frames the other passes
 * lose (measured on a 64 px tag: brightness x0.25, noise +-22: fast 1/6, coarse 5/6; x0.15: fast 0/6, coarse 4/6). It only reaches tags
 * with a full-resolution edge of roughly 48 px or more, so it is a fallback after the fast pass, not a replacement for it.
 */
export function detectAprilTagsCoarse(width: number, height: number, rgba: Uint8Array): DetectedAprilTag[] {
  if (width < 32 || height < 32 || rgba.length < width * height * 4) return [];
  const low = downscale(width, height, rgba, 4);
  const markers = detector().detectImage(low.width, low.height, low.rgba);
  return collectDetections(markers, (x, y) => [4 * x + 1.5, 4 * y + 1.5], TAG_DETECTOR_MIN_EDGE_PX / 4, width, height, rgba);
}

export type DetectionPass = 'full' | 'fast';
/** What actually produced a result: a 'fast' request falls back to the coarse pass when the half-size image shows nothing. */
export type DetectionPassUsed = 'full' | 'fast' | 'coarse';
/** A full pass every Nth frame (and the first), so a far or small tag is never missed for long. */
export const TAG_FULL_PASS_EVERY = 4;
export function chooseDetectionPass(frameIndex: number, fullEvery: number = TAG_FULL_PASS_EVERY): DetectionPass {
  return frameIndex % fullEvery === 0 ? 'full' : 'fast';
}
export function detectAprilTagsPassDetailed(pass: DetectionPass, width: number, height: number, rgba: Uint8Array): { detections: DetectedAprilTag[]; used: DetectionPassUsed } {
  if (pass === 'full') return { detections: detectAprilTags(width, height, rgba), used: 'full' };
  const fast = detectAprilTagsFast(width, height, rgba);
  if (fast.length > 0) return { detections: fast, used: 'fast' };
  const coarse = detectAprilTagsCoarse(width, height, rgba);
  return coarse.length > 0 ? { detections: coarse, used: 'coarse' } : { detections: fast, used: 'fast' };
}
export function detectAprilTagsPass(pass: DetectionPass, width: number, height: number, rgba: Uint8Array): DetectedAprilTag[] {
  return detectAprilTagsPassDetailed(pass, width, height, rgba).detections;
}

function quadArea(corners: DetectedAprilTag['corners']): number {
  let twice = 0;
  for (let i = 0; i < 4; i += 1) {
    const [x0, y0] = corners[i]; const [x1, y1] = corners[(i + 1) % 4];
    twice += x0 * y1 - x1 * y0;
  }
  return Math.abs(twice) / 2;
}

/**
 * Each tag id is one physical sticker, so a frame holds it at most once. The contour search can report the same sticker twice (the
 * outer border and a quad a couple of pixels inside it, seen at 1280x720 from a 15 degree view); keeping both double-counts one
 * observation with a slightly different pose. Keep the cleaner reading: fewest corrected bits, then the larger quad.
 */
function onePerTagId(detections: DetectedAprilTag[]): DetectedAprilTag[] {
  const best = new Map<number, DetectedAprilTag>();
  for (const detection of detections) {
    const held = best.get(detection.tagId);
    if (held === undefined || detection.hammingDistance < held.hammingDistance ||
        (detection.hammingDistance === held.hammingDistance && quadArea(detection.corners) > quadArea(held.corners))) {
      best.set(detection.tagId, detection);
    }
  }
  return detections.filter((detection) => best.get(detection.tagId) === detection);
}
