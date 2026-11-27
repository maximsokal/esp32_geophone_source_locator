import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { join } from 'node:path';

import { readDepthPng } from './pngDepth';

/**
 * Depth/pose replay of public RGB-D sequences (TUM RGB-D, ICL-NUIM TUM-compatible, Redwood) as the depth frames the scanner's fusion consumes:
 * metric z-depth on a phone-sized grid (default 160x120), a WebXR-style projection matrix and a world-from-view matrix in the WebXR frame (Y up).
 * The datasets are Kinect/synthetic depth, NOT ARCore depth: they stand for "an independent depth stream with ground-truth poses", nothing more.
 */
export interface ReplayFrame {
  timestamp: number;
  width: number;
  height: number;
  /** z-depth in metres, NaN where missing; row-major, y down. */
  depth: Float32Array;
  projection: Float32Array;
  /** column-major 4x4, camera -> WebXR world (Y up), camera looking down -z. */
  worldFromView: Float32Array;
}

export interface Intrinsics { fx: number; fy: number; cx: number; cy: number; width: number; height: number }

export const DATASET_ROOT = process.env.GEOLOCATOR_DATASETS ?? 'E:/datasets/3d-scanning';

type Mat = number[]; // row-major 4x4
const mul = (a: Mat, b: Mat): Mat => { const r = new Array<number>(16).fill(0); for (let i = 0; i < 4; i += 1) for (let j = 0; j < 4; j += 1) for (let k = 0; k < 4; k += 1) r[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j]; return r; };
const columnMajor = (m: Mat): Float32Array => Float32Array.from([m[0], m[4], m[8], m[12], m[1], m[5], m[9], m[13], m[2], m[6], m[10], m[14], m[3], m[7], m[11], m[15]]);
/** TUM/ICL camera axes (x right, y down, z forward) -> WebXR camera axes (x right, y up, z backward). */
const CAMERA_FLIP: Mat = [1, 0, 0, 0, 0, -1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1];
/** Dataset world (z up) -> WebXR world (y up). */
const WORLD_Y_UP: Mat = [1, 0, 0, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1];

function quaternionMatrix(tx: number, ty: number, tz: number, qx: number, qy: number, qz: number, qw: number): Mat {
  const n = Math.hypot(qx, qy, qz, qw) || 1; qx /= n; qy /= n; qz /= n; qw /= n;
  return [
    1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw), tx,
    2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw), ty,
    2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy), tz,
    0, 0, 0, 1,
  ];
}

export function projectionFor(k: Intrinsics): Float32Array {
  const m = new Float32Array(16);
  m[0] = (2 * k.fx) / k.width; m[5] = (2 * Math.abs(k.fy)) / k.height; m[8] = 1 - (2 * k.cx) / k.width; m[9] = (2 * k.cy) / k.height - 1;
  m[10] = -1; m[11] = -1; m[14] = -0.02;
  return m;
}

function readRows(path: string): Array<{ t: number; parts: string[] }> {
  return readFileSync(path, 'utf8').split(/\r?\n/).filter((l) => l.length > 0 && !l.startsWith('#')).map((l) => { const parts = l.trim().split(/\s+/); return { t: Number(parts[0]), parts }; });
}

export interface TumLikeOptions {
  /** directory that holds depth.txt, groundtruth.txt and depth/ */
  dir: string;
  intrinsics: Intrinsics;
  depthScale: number;
  /** ICL-NUIM stores the distance along the viewing ray; convert to z. */
  rayDistance?: boolean;
  /** use every n-th frame */
  stride?: number;
  maxFrames?: number;
  outWidth?: number;
  outHeight?: number;
  maxPairDt?: number;
}

export function datasetAvailable(dir: string): boolean { return existsSync(join(dir, 'depth.txt')) && existsSync(join(dir, 'groundtruth.txt')); }

/** Finds the directory that contains depth.txt below `root` (the archives unpack into one extra folder). */
export function findSequenceDir(root: string): string | null {
  if (!existsSync(root)) return null;
  if (existsSync(join(root, 'depth.txt'))) return root;
  for (const entry of readdirSync(root, { withFileTypes: true })) if (entry.isDirectory()) { const found = findSequenceDir(join(root, entry.name)); if (found !== null) return found; }
  return null;
}

export function loadTumLike(options: TumLikeOptions): ReplayFrame[] {
  const { dir, intrinsics: k, depthScale } = options;
  const outW = options.outWidth ?? 160; const outH = options.outHeight ?? 120;
  const depthList = readRows(join(dir, 'depth.txt'));
  const poses = readRows(join(dir, 'groundtruth.txt'));
  const maxDt = options.maxPairDt ?? 0.02;
  const frames: ReplayFrame[] = [];
  let pose = 0;
  const projection = projectionFor({ ...k, width: k.width, height: k.height });
  for (let i = 0; i < depthList.length; i += options.stride ?? 1) {
    const { t, parts } = depthList[i];
    while (pose + 1 < poses.length && Math.abs(poses[pose + 1].t - t) <= Math.abs(poses[pose].t - t)) pose += 1;
    if (poses.length === 0 || Math.abs(poses[pose].t - t) > maxDt) continue;
    const q = poses[pose].parts.slice(1).map(Number);
    const png = readDepthPng(join(dir, parts[1]));
    const depth = new Float32Array(outW * outH);
    for (let y = 0; y < outH; y += 1) for (let x = 0; x < outW; x += 1) {
      const sx = Math.min(png.width - 1, Math.floor(((x + 0.5) * png.width) / outW)); const sy = Math.min(png.height - 1, Math.floor(((y + 0.5) * png.height) / outH));
      const raw = png.values[sy * png.width + sx];
      let z = raw === 0 ? Number.NaN : raw / depthScale;
      if (options.rayDistance && Number.isFinite(z)) { const rx = (sx - k.cx) / k.fx; const ry = (sy - k.cy) / Math.abs(k.fy); z /= Math.sqrt(1 + rx * rx + ry * ry); }
      depth[y * outW + x] = z;
    }
    const camera = quaternionMatrix(q[0], q[1], q[2], q[3], q[4], q[5], q[6]);
    frames.push({ timestamp: t, width: outW, height: outH, depth, projection, worldFromView: columnMajor(mul(WORLD_Y_UP, mul(camera, CAMERA_FLIP))) });
    if (options.maxFrames !== undefined && frames.length >= options.maxFrames) break;
  }
  return frames;
}

export const TUM_INTRINSICS: Record<'freiburg1' | 'freiburg2', Intrinsics> = {
  freiburg1: { fx: 517.3, fy: 516.5, cx: 318.6, cy: 255.3, width: 640, height: 480 },
  freiburg2: { fx: 520.9, fy: 521.0, cx: 325.1, cy: 249.7, width: 640, height: 480 },
};
export const ICL_INTRINSICS: Intrinsic = { fx: 481.2, fy: -480.0, cx: 319.5, cy: 239.5, width: 640, height: 480 };
type Intrinsic = Intrinsics;

/** The depth image as the accumulator reads it: nearest pixel by normalised coordinates, metres, NaN when missing. */
export function depthInformation(frame: ReplayFrame): { width: number; height: number; getDepthInMeters(x: number, y: number): number; projectionMatrix: Float32Array; transform: { matrix: Float32Array } } {
  const { width, height, depth } = frame;
  return {
    width, height,
    getDepthInMeters: (x: number, y: number) => { const px = Math.min(width - 1, Math.max(0, Math.trunc(x * width))); const py = Math.min(height - 1, Math.max(0, Math.trunc(y * height))); return depth[py * width + px]; },
    projectionMatrix: frame.projection,
    transform: { matrix: frame.worldFromView },
  };
}

/** Camera position in the project frame (Z up): project = [x, -z, y] of the WebXR world. */
export function projectCameraPosition(frame: ReplayFrame): [number, number, number] {
  const m = frame.worldFromView; return [m[12], -m[14], m[13]];
}

function downsample(png: { width: number; height: number; values: Uint16Array }, outW: number, outH: number, depthScale: number): Float32Array {
  const depth = new Float32Array(outW * outH);
  for (let y = 0; y < outH; y += 1) for (let x = 0; x < outW; x += 1) {
    const sx = Math.min(png.width - 1, Math.floor(((x + 0.5) * png.width) / outW)); const sy = Math.min(png.height - 1, Math.floor(((y + 0.5) * png.height) / outH));
    const raw = png.values[sy * png.width + sx];
    depth[y * outW + x] = raw === 0 ? Number.NaN : raw / depthScale;
  }
  return depth;
}

export interface FolderReplayOptions { stride?: number; maxFrames?: number; outWidth?: number; outHeight?: number }

/**
 * ICL-NUIM TUM-compatible sequences: depth/<n>.png (16-bit, scale 5000, z-depth: verified by plane fit, see the manifest) and a Freiburg ground-truth file
 * "index tx ty tz qx qy qz qw" whose rows are the frames in order.
 */
export function loadIclSequence(dir: string, groundTruthFile: string, options: FolderReplayOptions = {}): ReplayFrame[] {
  const outW = options.outWidth ?? 160; const outH = options.outHeight ?? 120;
  const files = readdirSync(join(dir, 'depth')).filter((name) => name.endsWith('.png')).sort((a, b) => Number.parseInt(a, 10) - Number.parseInt(b, 10));
  const poses = readRows(groundTruthFile);
  const projection = projectionFor(ICL_INTRINSICS);
  const frames: ReplayFrame[] = [];
  const count = Math.min(files.length, poses.length);
  for (let i = 0; i < count; i += options.stride ?? 1) {
    const q = poses[i].parts.slice(1).map(Number);
    const png = readDepthPng(join(dir, 'depth', files[i]));
    const camera = quaternionMatrix(q[0], q[1], q[2], q[3], q[4], q[5], q[6]);
    frames.push({ timestamp: i / 30, width: outW, height: outH, depth: downsample(png, outW, outH, 5000), projection, worldFromView: columnMajor(mul(WORLD_Y_UP, mul(camera, CAMERA_FLIP))) });
    if (options.maxFrames !== undefined && frames.length >= options.maxFrames) break;
  }
  return frames;
}

/** Redwood augmented ICL-NUIM: <depthDir>/NNNNN.png (16-bit millimetres, z-depth) and a trajectory file of "i i+1 n" headers each followed by a 4x4 camera-to-world matrix. */
export function loadRedwoodSequence(depthDir: string, trajectoryFile: string, options: FolderReplayOptions = {}): ReplayFrame[] {
  const outW = options.outWidth ?? 160; const outH = options.outHeight ?? 120;
  const lines = readFileSync(trajectoryFile, 'utf8').split(/\r?\n/).filter((l) => l.trim().length > 0);
  const matrices: Mat[] = [];
  for (let i = 0; i + 4 < lines.length + 1; i += 5) {
    const rows = lines.slice(i + 1, i + 5).map((l) => l.trim().split(/\s+/).map(Number));
    if (rows.length === 4 && rows.every((r) => r.length === 4)) matrices.push(rows.flat());
  }
  const files = readdirSync(depthDir).filter((name) => name.endsWith('.png')).sort();
  const projection = projectionFor(ICL_INTRINSICS);
  const frames: ReplayFrame[] = [];
  const count = Math.min(files.length, matrices.length);
  for (let i = 0; i < count; i += options.stride ?? 1) {
    const png = readDepthPng(join(depthDir, files[i]));
    frames.push({ timestamp: i / 30, width: outW, height: outH, depth: downsample(png, outW, outH, 1000), projection, worldFromView: columnMajor(mul(WORLD_Y_UP, mul(matrices[i], CAMERA_FLIP))) });
    if (options.maxFrames !== undefined && frames.length >= options.maxFrames) break;
  }
  return frames;
}
