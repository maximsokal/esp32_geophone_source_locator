/**
 * MAX-251: geometrically consistent multi-view synthetic depth harness.
 *
 * Test-only (never bundled). Scenes are analytic convex polygons in the
 * PROJECT frame used by the accumulator (metres, +Z up; project = [xrX, -xrZ, xrY]).
 * Every pose renders its own pixelised depth map by ray casting the scene from
 * a real look-at camera, and the depth callback reproduces the runtime sampler
 * semantics (nearest pixel via trunc, view-space -z in metres; see
 * src/xr/cpuDepthSampler.ts). Poses therefore produce real parallax, occlusion
 * and grazing-angle effects.
 *
 * Noise model (seeded, per pose and pixel, independent of call order):
 *   sigma(d) = 0.0015 * d^2 m (ARCore-like), then 1 mm quantisation;
 *   dropout (NaN) when the incidence angle between ray and face normal > 80 deg;
 *   optional mixed pixels: 2x2 sub-pixel rays whose depths differ by > 5 cm are
 *   averaged, like foreground/background mixing on a discontinuity.
 */

import type {
  CpuDepthInformationLike,
  DepthFusionFrameContext,
  DepthPointCloudAccumulator,
  DepthSampleView,
} from '../../../src/xr/depthPointCloudV2';

export type Vec3 = readonly [number, number, number];

export interface ScenePolygon {
  /** Convex, counter-clockwise when seen from the side the normal points to. */
  vertices: readonly Vec3[];
  /** Outward unit normal (the side that is visible / physically exposed). */
  normal: Vec3;
  label: string;
}

export interface SceneCrease {
  start: Vec3;
  end: Vec3;
  label: string;
}

export interface SyntheticScene {
  name: string;
  polygons: readonly ScenePolygon[];
  creases: readonly SceneCrease[];
  /** Analytic 3-face corners (vertex positions), if any. */
  corners: readonly Vec3[];
  /** Point the default camera rigs look at. */
  focus: Vec3;
}

// ---------------------------------------------------------------- vector math
export function add(a: Vec3, b: Vec3): Vec3 { return [a[0] + b[0], a[1] + b[1], a[2] + b[2]]; }
export function sub(a: Vec3, b: Vec3): Vec3 { return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]; }
export function scale(a: Vec3, s: number): Vec3 { return [a[0] * s, a[1] * s, a[2] * s]; }
export function dot(a: Vec3, b: Vec3): number { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
export function cross(a: Vec3, b: Vec3): Vec3 {
  return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
}
export function norm(a: Vec3): number { return Math.hypot(a[0], a[1], a[2]); }
export function normalize(a: Vec3): Vec3 { const n = norm(a); return [a[0] / n, a[1] / n, a[2] / n]; }
const DEG = Math.PI / 180;

// ------------------------------------------------------------- scene builders
/** Rectangle centred at `centre` spanned by half-axes `u`, `v`; normal = normalize(u x v). */
export function rectangle(centre: Vec3, u: Vec3, v: Vec3, label: string): ScenePolygon {
  const normal = normalize(cross(u, v));
  return {
    vertices: [
      sub(sub(centre, u), v), sub(add(centre, u), v), add(add(centre, u), v), add(sub(centre, u), v),
    ],
    normal,
    label,
  };
}

/**
 * Convex box corner seen from outside: faces x = c.x, y = c.y, z = c.z of an
 * axis-aligned box of edge `size` whose maximal corner is `corner`.
 */
export function boxCornerScene(corner: Vec3 = [0, 0, 0], size = 0.4): SyntheticScene {
  const h = size / 2;
  const [cx, cy, cz] = corner;
  return {
    name: 'box-corner',
    polygons: [
      rectangle([cx, cy - h, cz - h], [0, h, 0], [0, 0, h], 'face+x'),
      rectangle([cx - h, cy, cz - h], [0, 0, h], [h, 0, 0], 'face+y'),
      rectangle([cx - h, cy - h, cz], [h, 0, 0], [0, h, 0], 'face+z'),
    ],
    creases: [
      { start: [cx, cy, cz - size], end: corner, label: 'x|y' },
      { start: [cx, cy - size, cz], end: corner, label: 'x|z' },
      { start: [cx - size, cy, cz], end: corner, label: 'y|z' },
    ],
    corners: [corner],
    focus: [cx - size * 0.25, cy - size * 0.25, cz - size * 0.25],
  };
}

/** Concave inside corner: walls x = c.x, y = c.y and floor z = c.z meeting at `corner`. */
export function insideCornerScene(corner: Vec3 = [0, 0, 0], size = 0.6): SyntheticScene {
  const h = size / 2;
  const [cx, cy, cz] = corner;
  return {
    name: 'inside-corner',
    polygons: [
      rectangle([cx, cy + h, cz + h], [0, h, 0], [0, 0, h], 'wall+x'),
      rectangle([cx + h, cy, cz + h], [0, 0, h], [h, 0, 0], 'wall+y'),
      rectangle([cx + h, cy + h, cz], [h, 0, 0], [0, h, 0], 'floor+z'),
    ],
    creases: [
      { start: corner, end: [cx, cy, cz + size], label: 'x|y' },
      { start: corner, end: [cx, cy + size, cz], label: 'x|z' },
      { start: corner, end: [cx + size, cy, cz], label: 'y|z' },
    ],
    corners: [corner],
    focus: [cx + size * 0.25, cy + size * 0.25, cz + size * 0.25],
  };
}

/**
 * Convex ridge along the project X axis through `apex` with interior dihedral
 * `dihedralDeg` (180 = flat). Faces fall away from the ridge on both sides.
 */
export function ridgeScene(dihedralDeg: number, apex: Vec3 = [0, 0, 0], length = 0.6, width = 0.4): SyntheticScene {
  const half = (dihedralDeg / 2) * DEG;
  const dirA: Vec3 = [0, Math.sin(half), -Math.cos(half)];
  const dirB: Vec3 = [0, -Math.sin(half), -Math.cos(half)];
  const along: Vec3 = [length / 2, 0, 0];
  const faceA = rectangle(add(apex, scale(dirA, width / 2)), along, scale(dirA, width / 2), 'ridgeA');
  const faceB = rectangle(add(apex, scale(dirB, width / 2)), scale(dirB, width / 2), along, 'ridgeB');
  return {
    name: `ridge-${dihedralDeg}`,
    polygons: [orientOutward(faceA, [0, Math.cos(half), Math.sin(half)]), orientOutward(faceB, [0, -Math.cos(half), Math.sin(half)])],
    creases: [{ start: sub(apex, along), end: add(apex, along), label: 'ridge' }],
    corners: [],
    focus: add(apex, [0, 0, -width * 0.15]),
  };
}

/** Concave V-notch along X: faces rise from the valley line at `bottom`. */
export function vNotchScene(dihedralDeg: number, bottom: Vec3 = [0, 0, 0], length = 0.6, width = 0.4): SyntheticScene {
  const half = (dihedralDeg / 2) * DEG;
  const dirA: Vec3 = [0, Math.sin(half), Math.cos(half)];
  const dirB: Vec3 = [0, -Math.sin(half), Math.cos(half)];
  const along: Vec3 = [length / 2, 0, 0];
  const faceA = rectangle(add(bottom, scale(dirA, width / 2)), along, scale(dirA, width / 2), 'notchA');
  const faceB = rectangle(add(bottom, scale(dirB, width / 2)), scale(dirB, width / 2), along, 'notchB');
  return {
    name: `vnotch-${dihedralDeg}`,
    polygons: [orientOutward(faceA, [0, -Math.cos(half), Math.sin(half)]), orientOutward(faceB, [0, Math.cos(half), Math.sin(half)])],
    creases: [{ start: sub(bottom, along), end: add(bottom, along), label: 'valley' }],
    corners: [],
    focus: add(bottom, [0, 0, width * 0.15]),
  };
}

/** Thin horizontal slab: top, bottom and the +X rim face. */
export function slabScene(thickness = 0.02, size = 0.5, centre: Vec3 = [0, 0, 0]): SyntheticScene {
  const t = thickness / 2;
  const h = size / 2;
  const [cx, cy, cz] = centre;
  return {
    name: 'slab',
    polygons: [
      rectangle([cx, cy, cz + t], [h, 0, 0], [0, h, 0], 'top'),
      rectangle([cx, cy, cz - t], [0, h, 0], [h, 0, 0], 'bottom'),
      rectangle([cx + h, cy, cz], [0, h, 0], [0, 0, t], 'rim'),
    ],
    creases: [
      { start: [cx + h, cy - h, cz + t], end: [cx + h, cy + h, cz + t], label: 'top|rim' },
      { start: [cx + h, cy - h, cz - t], end: [cx + h, cy + h, cz - t], label: 'bottom|rim' },
    ],
    corners: [],
    focus: [cx + h * 0.6, cy, cz],
  };
}

/** Step: lower floor (x < 0) at z = 0, upper floor (x > 0) at z = height, riser facing -X. */
export function stepScene(height = 0.2, size = 0.6): SyntheticScene {
  const h = size / 2;
  return {
    name: 'step',
    polygons: [
      rectangle([-h, 0, 0], [h, 0, 0], [0, h, 0], 'lower'),
      rectangle([h, 0, height], [h, 0, 0], [0, h, 0], 'upper'),
      rectangle([0, 0, height / 2], [0, 0, height / 2], [0, h, 0], 'riser'),
    ].map((polygon) => polygon.label === 'riser' ? orientOutward(polygon, [-1, 0, 0]) : polygon),
    creases: [
      { start: [0, -h, height], end: [0, h, height], label: 'upper|riser' },
      { start: [0, -h, 0], end: [0, h, 0], label: 'lower|riser' },
    ],
    corners: [],
    focus: [0, 0, height / 2],
  };
}

/** Square plane of edge `size` centred at `centre`; normal = Rz(yaw) Ry(tilt) (+Z). */
export function tiltedPlaneScene(tiltDeg: number, yawDeg = 0, size = 0.8, centre: Vec3 = [0, 0, 0]): SyntheticScene {
  const t = tiltDeg * DEG;
  const y = yawDeg * DEG;
  const rot = (v: Vec3): Vec3 => {
    const x1 = v[0] * Math.cos(t) + v[2] * Math.sin(t);
    const z1 = -v[0] * Math.sin(t) + v[2] * Math.cos(t);
    return [x1 * Math.cos(y) - v[1] * Math.sin(y), x1 * Math.sin(y) + v[1] * Math.cos(y), z1];
  };
  return {
    name: `plane-${tiltDeg}-${yawDeg}`,
    polygons: [rectangle(centre, rot([size / 2, 0, 0]), rot([0, size / 2, 0]), 'plane')],
    creases: [],
    corners: [],
    focus: centre,
  };
}

function orientOutward(polygon: ScenePolygon, outward: Vec3): ScenePolygon {
  if (dot(polygon.normal, outward) >= 0) return polygon;
  return { ...polygon, vertices: [...polygon.vertices].reverse(), normal: scale(polygon.normal, -1) };
}

// ----------------------------------------------------------------- ray casting
interface Hit { t: number; polygon: ScenePolygon; point: Vec3 }

function insideConvex(polygon: ScenePolygon, point: Vec3): boolean {
  const n = polygon.vertices.length;
  let sign = 0;
  for (let i = 0; i < n; i += 1) {
    const a = polygon.vertices[i];
    const b = polygon.vertices[(i + 1) % n];
    const side = dot(cross(sub(b, a), sub(point, a)), polygon.normal);
    if (Math.abs(side) < 1e-12) continue;
    const s = Math.sign(side);
    if (sign === 0) sign = s; else if (s !== sign) return false;
  }
  return true;
}

export function castRay(scene: SyntheticScene, origin: Vec3, direction: Vec3): Hit | null {
  let best: Hit | null = null;
  for (const polygon of scene.polygons) {
    const denom = dot(polygon.normal, direction);
    if (Math.abs(denom) < 1e-12) continue;
    const t = dot(polygon.normal, sub(polygon.vertices[0], origin)) / denom;
    if (!(t > 1e-6) || (best !== null && t >= best.t)) continue;
    const point = add(origin, scale(direction, t));
    if (insideConvex(polygon, point)) best = { t, polygon, point };
  }
  return best;
}

// ------------------------------------------------------------------- cameras
export interface CameraPose {
  /** Eye position in the project frame. */
  eye: Vec3;
  /** Look-at target in the project frame. */
  target: Vec3;
}

export interface CameraIntrinsics {
  width: number;
  height: number;
  /** Vertical field of view in degrees. */
  fovYDeg: number;
}

export const DEFAULT_INTRINSICS: CameraIntrinsics = { width: 160, height: 90, fovYDeg: 60 };

/** project [x, y, z] -> XR world [x, z, -y] (inverse of the runtime's [xrX, -xrZ, xrY]). */
export function projectToWorld(p: Vec3): Vec3 { return [p[0], p[2], -p[1]]; }
export function worldToProject(w: Vec3): Vec3 { return [w[0], -w[2], w[1]]; }

export function projectionMatrix(intrinsics: CameraIntrinsics): Float32Array {
  const p11 = 1 / Math.tan((intrinsics.fovYDeg * DEG) / 2);
  const p00 = p11 / (intrinsics.width / intrinsics.height);
  // Column-major; only [0], [5], [8], [9] are read by the accumulator.
  return new Float32Array([p00, 0, 0, 0, 0, p11, 0, 0, 0, 0, -1, -1, 0, 0, -0.2, 0]);
}

/** Column-major worldFromView for a look-at camera (views look down -Z, world +Y up). */
export function worldFromView(pose: CameraPose): Float32Array {
  const eye = projectToWorld(pose.eye);
  const target = projectToWorld(pose.target);
  const forward = normalize(sub(target, eye));
  const worldUp: Vec3 = Math.abs(forward[1]) > 0.999 ? [0, 0, -1] : [0, 1, 0];
  const right = normalize(cross(forward, worldUp));
  const up = cross(right, forward);
  return new Float32Array([
    right[0], right[1], right[2], 0,
    up[0], up[1], up[2], 0,
    -forward[0], -forward[1], -forward[2], 0,
    eye[0], eye[1], eye[2], 1,
  ]);
}

/** Pixel-centre ray in the PROJECT frame for normalized image coordinates (u, v). */
export function viewRay(pose: CameraPose, intrinsics: CameraIntrinsics, u: number, v: number): Vec3 {
  const projection = projectionMatrix(intrinsics);
  const m = worldFromView(pose);
  const vx = (u * 2 - 1) / projection[0];
  const vy = (1 - v * 2) / projection[5];
  const vz = -1;
  const world: Vec3 = [m[0] * vx + m[4] * vy + m[8] * vz, m[1] * vx + m[5] * vy + m[9] * vz, m[2] * vx + m[6] * vy + m[10] * vz];
  return worldToProject(world);
}

/** Normalized image coordinates of a project-frame point (null when behind the camera). */
export function projectToImage(pose: CameraPose, intrinsics: CameraIntrinsics, point: Vec3): [number, number] | null {
  const projection = projectionMatrix(intrinsics);
  const m = worldFromView(pose);
  const w = sub(projectToWorld(point), [m[12], m[13], m[14]]);
  const vx = w[0] * m[0] + w[1] * m[1] + w[2] * m[2];
  const vy = w[0] * m[4] + w[1] * m[5] + w[2] * m[6];
  const vz = w[0] * m[8] + w[1] * m[9] + w[2] * m[10];
  if (vz >= 0) return null;
  const depth = -vz;
  return [((vx * projection[0]) / depth + 1) / 2, (1 - (vy * projection[5]) / depth) / 2];
}

/** Orbit around `target` at `radius`, `elevationDeg` above the target, azimuths evenly spread. */
export function orbitPoses(target: Vec3, radius: number, elevationDeg: number, azimuthFromDeg: number, azimuthToDeg: number, count: number): CameraPose[] {
  const poses: CameraPose[] = [];
  for (let i = 0; i < count; i += 1) {
    const a = (azimuthFromDeg + (count === 1 ? 0 : (azimuthToDeg - azimuthFromDeg) * i / (count - 1))) * DEG;
    const e = elevationDeg * DEG;
    poses.push({
      eye: [target[0] + radius * Math.cos(e) * Math.cos(a), target[1] + radius * Math.cos(e) * Math.sin(a), target[2] + radius * Math.sin(e)],
      target,
    });
  }
  return poses;
}

// --------------------------------------------------------------------- noise
function hash32(value: number): number {
  let h = value | 0;
  h = Math.imul(h ^ (h >>> 16), 0x45d9f3b);
  h = Math.imul(h ^ (h >>> 16), 0x45d9f3b);
  return (h ^ (h >>> 16)) >>> 0;
}
function unit(seed: number, a: number, b: number, c: number, salt: number): number {
  const h = hash32(seed * 0x9e3779b1 + hash32(a * 73856093 ^ b * 19349663 ^ c * 83492791 ^ salt * 2654435761));
  return (h + 0.5) / 4294967296;
}
function gaussian(seed: number, a: number, b: number, c: number): number {
  const u1 = unit(seed, a, b, c, 1);
  const u2 = unit(seed, a, b, c, 2);
  return Math.sqrt(-2 * Math.log(u1)) * Math.cos(2 * Math.PI * u2);
}

export interface NoiseOptions {
  /** 0 disables noise and quantisation (exact depth). */
  seed: number;
  enabled: boolean;
  mixedPixels?: boolean;
  /** Incidence angle above which the sensor returns no depth. */
  dropoutIncidenceDeg?: number;
}

export const NO_NOISE: NoiseOptions = { seed: 0, enabled: false, dropoutIncidenceDeg: 90 };
export const ARCORE_NOISE: NoiseOptions = { seed: 20260924, enabled: true, dropoutIncidenceDeg: 80 };

export function depthSigmaMeters(depthMeters: number): number { return 0.0015 * depthMeters * depthMeters; }

// ---------------------------------------------------------------- rendering
export interface RenderedDepthFrame {
  depth: CpuDepthInformationLike;
  view: DepthSampleView;
  pose: CameraPose;
  /** Raw buffer (row-major, metres, NaN = no depth). */
  buffer: Float32Array;
}

export function renderDepth(
  scene: SyntheticScene,
  pose: CameraPose,
  poseIndex: number,
  noise: NoiseOptions = NO_NOISE,
  intrinsics: CameraIntrinsics = DEFAULT_INTRINSICS,
): RenderedDepthFrame {
  const { width, height } = intrinsics;
  const projection = projectionMatrix(intrinsics);
  const transform = worldFromView(pose);
  const buffer = new Float32Array(width * height);
  const dropout = Math.cos((noise.dropoutIncidenceDeg ?? 90) * DEG);
  const depthAlong = (u: number, v: number): { depth: number; incidenceCos: number } | null => {
    const ray = viewRay(pose, intrinsics, u, v);
    const hit = castRay(scene, pose.eye, ray);
    if (hit === null) return null;
    // Ray is (ndc/p, -1) in view space, so the hit parameter t IS the view-space depth.
    const incidenceCos = Math.abs(dot(normalize(ray), hit.polygon.normal));
    return { depth: hit.t, incidenceCos };
  };
  for (let row = 0; row < height; row += 1) {
    for (let column = 0; column < width; column += 1) {
      const u = (column + 0.5) / width;
      const v = (row + 0.5) / height;
      let sample = depthAlong(u, v);
      if (sample !== null && noise.mixedPixels === true) {
        const subs = [[-0.25, -0.25], [0.25, -0.25], [-0.25, 0.25], [0.25, 0.25]]
          .map(([du, dv]) => depthAlong(u + du / width, v + dv / height))
          .filter((s): s is { depth: number; incidenceCos: number } => s !== null);
        if (subs.length > 0) {
          const min = Math.min(...subs.map((s) => s.depth));
          const max = Math.max(...subs.map((s) => s.depth));
          if (max - min > 0.05) sample = { depth: subs.reduce((sum, s) => sum + s.depth, 0) / subs.length, incidenceCos: sample.incidenceCos };
        }
      }
      let value = Number.NaN;
      if (sample !== null && sample.incidenceCos >= dropout) {
        value = sample.depth;
        if (noise.enabled) {
          value += gaussian(noise.seed, poseIndex, row, column) * depthSigmaMeters(value);
          value = Math.round(value * 1000) / 1000;
        }
      }
      buffer[row * width + column] = value;
    }
  }
  const depth: CpuDepthInformationLike = {
    width,
    height,
    projectionMatrix: projection,
    transform: { matrix: transform },
    getDepthInMeters(x: number, y: number): number {
      // Same nearest-pixel lookup as src/xr/cpuDepthSampler.ts (identity normDepthBufferFromNormView).
      const column = Math.max(0, Math.min(width - 1, Math.trunc(x * width)));
      const row = Math.max(0, Math.min(height - 1, Math.trunc(y * height)));
      return buffer[row * width + column];
    },
  };
  return { depth, view: { projectionMatrix: projection, transform: { matrix: transform } }, pose, buffer };
}

export interface ScanOptions {
  noise?: NoiseOptions;
  intrinsics?: CameraIntrinsics;
  /** Extra non-evidence polish frames rendered from each pose. */
  polishFramesPerPose?: number;
  firstKeyframeId?: number;
}

/** Feed every pose as an evidence keyframe (keyframeId = index + 1). */
export function scanScene(
  accumulator: DepthPointCloudAccumulator,
  scene: SyntheticScene,
  poses: readonly CameraPose[],
  options: ScanOptions = {},
): { frames: number; featureProbes: number; edgeProbes: number } {
  const noise = options.noise ?? NO_NOISE;
  const intrinsics = options.intrinsics ?? DEFAULT_INTRINSICS;
  const first = options.firstKeyframeId ?? 1;
  let frames = 0; let featureProbes = 0; let edgeProbes = 0;
  poses.forEach((pose, index) => {
    const frame = renderDepth(scene, pose, first + index, noise, intrinsics);
    const context: DepthFusionFrameContext = {
      keyframeId: first + index, evidenceKeyframe: true, frameWeight: 1, cameraProjectPosition: pose.eye,
    };
    const result = accumulator.sampleDepthFrame(frame.depth, frame.view, context);
    featureProbes += result.featureSupersampleProbes; edgeProbes += result.edgeSupersampleProbes; frames += 1;
    for (let polish = 0; polish < (options.polishFramesPerPose ?? 0); polish += 1) {
      accumulator.sampleDepthFrame(frame.depth, frame.view, { ...context, evidenceKeyframe: false, frameWeight: 0.2 });
      frames += 1;
    }
  });
  return { frames, featureProbes, edgeProbes };
}

// ------------------------------------------------------------------- metrics
function closestOnSegment(point: Vec3, a: Vec3, b: Vec3): Vec3 {
  const ab = sub(b, a);
  const t = Math.max(0, Math.min(1, dot(sub(point, a), ab) / Math.max(1e-12, dot(ab, ab))));
  return add(a, scale(ab, t));
}

export function distanceToPolygon(polygon: ScenePolygon, point: Vec3): number {
  const offset = dot(sub(point, polygon.vertices[0]), polygon.normal);
  const onPlane = sub(point, scale(polygon.normal, offset));
  if (insideConvex(polygon, onPlane)) return Math.abs(offset);
  let best = Number.POSITIVE_INFINITY;
  const n = polygon.vertices.length;
  for (let i = 0; i < n; i += 1) {
    best = Math.min(best, norm(sub(point, closestOnSegment(point, polygon.vertices[i], polygon.vertices[(i + 1) % n]))));
  }
  return best;
}

export function nearestPolygon(scene: SyntheticScene, point: Vec3): { polygon: ScenePolygon; distance: number } {
  let best = { polygon: scene.polygons[0], distance: Number.POSITIVE_INFINITY };
  for (const polygon of scene.polygons) {
    const distance = distanceToPolygon(polygon, point);
    if (distance < best.distance) best = { polygon, distance };
  }
  return best;
}

export function distanceToCrease(scene: SyntheticScene, point: Vec3): number {
  let best = Number.POSITIVE_INFINITY;
  for (const crease of scene.creases) best = Math.min(best, norm(sub(point, closestOnSegment(point, crease.start, crease.end))));
  return best;
}

/** Sign-less angle between a measured normal and the true one, degrees. */
export function normalErrorDeg(measured: Vec3, truth: Vec3): number {
  const length = norm(measured);
  if (!(length > 1e-9)) return 180;
  return Math.acos(Math.min(1, Math.abs(dot(measured, truth)) / length)) / DEG;
}

/**
 * A point within `ambiguityMeters` of several faces (on or next to a crease)
 * may legitimately carry any of their normals; the error is against the best
 * of them. 1 cm ~ 3 sigma of depth noise at 1.5 m plus fused position error.
 */
export const FACE_AMBIGUITY_METERS = 0.01;

export function sceneNormalErrorDeg(scene: SyntheticScene, point: Vec3, measured: Vec3, ambiguityMeters = FACE_AMBIGUITY_METERS): number {
  const nearest = nearestPolygon(scene, point).distance;
  let best = 180;
  for (const polygon of scene.polygons) {
    if (distanceToPolygon(polygon, point) <= nearest + ambiguityMeters) best = Math.min(best, normalErrorDeg(measured, polygon.normal));
  }
  return best;
}

export interface PointSet {
  positions: ArrayLike<number>;
  normals?: ArrayLike<number>;
  confidence?: ArrayLike<number>;
  count: number;
}

export function pointAt(set: PointSet, index: number): Vec3 {
  return [Number(set.positions[index * 3]), Number(set.positions[index * 3 + 1]), Number(set.positions[index * 3 + 2])];
}
export function normalAt(set: PointSet, index: number): Vec3 {
  const n = set.normals!;
  return [Number(n[index * 3]), Number(n[index * 3 + 1]), Number(n[index * 3 + 2])];
}

export function percentile(values: readonly number[], fraction: number): number {
  if (values.length === 0) return Number.NaN;
  const sorted = [...values].sort((a, b) => a - b);
  return sorted[Math.min(sorted.length - 1, Math.max(0, Math.round((sorted.length - 1) * fraction)))];
}

export interface CreaseBinStats {
  /** [min, max) distance to the nearest crease in voxels. */
  fromVoxels: number;
  toVoxels: number;
  count: number;
  meanSurfaceDistanceM: number;
  p95SurfaceDistanceM: number;
  meanNormalErrorDeg: number;
  p90NormalErrorDeg: number;
}

export const CREASE_BINS_VOXELS: readonly (readonly [number, number])[] = [[0, 1], [1, 2], [2, 5], [5, Number.POSITIVE_INFINITY]];

export function creaseBinStats(scene: SyntheticScene, set: PointSet, voxelSizeMeters: number, filter: (index: number) => boolean = () => true): CreaseBinStats[] {
  return CREASE_BINS_VOXELS.map(([from, to]) => {
    const distances: number[] = []; const errors: number[] = [];
    for (let i = 0; i < set.count; i += 1) {
      if (!filter(i)) continue;
      const p = pointAt(set, i);
      const crease = distanceToCrease(scene, p) / voxelSizeMeters;
      if (!(crease >= from && crease < to)) continue;
      const nearest = nearestPolygon(scene, p);
      distances.push(nearest.distance);
      if (set.normals !== undefined) errors.push(sceneNormalErrorDeg(scene, p, normalAt(set, i)));
    }
    const mean = (values: number[]): number => values.length === 0 ? Number.NaN : values.reduce((s, v) => s + v, 0) / values.length;
    return {
      fromVoxels: from, toVoxels: to, count: distances.length,
      meanSurfaceDistanceM: mean(distances), p95SurfaceDistanceM: percentile(distances, 0.95),
      meanNormalErrorDeg: mean(errors), p90NormalErrorDeg: percentile(errors, 0.9),
    };
  });
}

/** Points that would pass a structural confidence gate while carrying a wrong normal. */
export function wrongNormalPassingGate(scene: SyntheticScene, set: PointSet, gate: number, maxErrorDeg: number): number {
  let count = 0;
  for (let i = 0; i < set.count; i += 1) {
    if (Number(set.confidence![i]) < gate) continue;
    const p = pointAt(set, i);
    if (sceneNormalErrorDeg(scene, p, normalAt(set, i)) > maxErrorDeg) count += 1;
  }
  return count;
}

/** Total scene surface area and the part within `band` metres of any crease (deterministic grid integration). */
export function polygonAreaSplit(scene: SyntheticScene, band: number, resolution = 0.005): { total: number; nearCrease: number } {
  let total = 0; let nearCrease = 0;
  for (const polygon of scene.polygons) {
    const a = polygon.vertices[0];
    const e1 = sub(polygon.vertices[1], a);
    const e2 = sub(polygon.vertices[polygon.vertices.length - 1], a);
    const n1 = Math.max(1, Math.round(norm(e1) / resolution));
    const n2 = Math.max(1, Math.round(norm(e2) / resolution));
    const cell = (norm(e1) / n1) * (norm(e2) / n2);
    for (let i = 0; i < n1; i += 1) for (let j = 0; j < n2; j += 1) {
      const p = add(add(a, scale(e1, (i + 0.5) / n1)), scale(e2, (j + 0.5) / n2));
      if (!insideConvex(polygon, p)) continue;
      total += cell;
      if (distanceToCrease(scene, p) <= band) nearCrease += cell;
    }
  }
  return { total, nearCrease };
}
