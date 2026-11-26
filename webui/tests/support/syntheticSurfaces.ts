/*
 * Public, made-up scenes with a KNOWN true surface, to measure reconstruction quality (precision, recall, false bridges). Points are sampled the way a depth
 * camera sees a surface from outside: with the normal pointing out of the object.
 */
export interface SyntheticSurfaceScene {
  name: string;
  /** Measured surface points (project frame, metres), three floats each. */
  points: Float32Array;
  normals: Float32Array;
  /** The true surface sampled much more densely than the points, for precision and recall. */
  truth: Float32Array;
  /** Regions of space where NO surface exists and a mesh must not appear (an opening, the gap between two objects): centre and radius. */
  voids: ReadonlyArray<{ centre: readonly [number, number, number]; radius: number }>;
}

function random(seed: number): () => number {
  let state = seed >>> 0;
  return () => { state = (Math.imul(state, 1664525) + 1013904223) >>> 0; return state / 0x100000000; };
}
function gaussian(next: () => number): number { return Math.sqrt(-2 * Math.log(Math.max(1e-12, next()))) * Math.cos(2 * Math.PI * next()); }

interface Builder { points: number[]; normals: number[]; truth: number[] }
const builder = (): Builder => ({ points: [], normals: [], truth: [] });

/** A rectangle spanned by `u` and `v` from `origin`, sampled every `step` (points) and every `truthStep` (truth); the normal is n. */
function rectangle(b: Builder, origin: readonly number[], u: readonly number[], v: readonly number[], n: readonly number[], step: number, truthStep = 0.02, skip: (s: number, t: number) => boolean = () => false): void {
  const lu = Math.hypot(u[0], u[1], u[2]); const lv = Math.hypot(v[0], v[1], v[2]);
  for (let i = 0; i <= Math.round(lu / step); i += 1) for (let j = 0; j <= Math.round(lv / step); j += 1) {
    const s = i * step / lu; const t = j * step / lv;
    if (skip(s * lu, t * lv)) continue;
    b.points.push(origin[0] + u[0] * s + v[0] * t, origin[1] + u[1] * s + v[1] * t, origin[2] + u[2] * s + v[2] * t);
    b.normals.push(n[0], n[1], n[2]);
  }
  for (let i = 0; i <= Math.round(lu / truthStep); i += 1) for (let j = 0; j <= Math.round(lv / truthStep); j += 1) {
    const s = i * truthStep / lu; const t = j * truthStep / lv;
    if (skip(s * lu, t * lv)) continue;
    b.truth.push(origin[0] + u[0] * s + v[0] * t, origin[1] + u[1] * s + v[1] * t, origin[2] + u[2] * s + v[2] * t);
  }
}
function sphere(b: Builder, centre: readonly number[], radius: number, step: number): void {
  const rings = Math.round(Math.PI * radius / step);
  for (let a = 1; a < rings; a += 1) {
    const phi = Math.PI * a / rings; const ringRadius = Math.sin(phi) * radius;
    const around = Math.max(6, Math.round(2 * Math.PI * ringRadius / step));
    for (let c = 0; c < around; c += 1) {
      const theta = 2 * Math.PI * c / around;
      const n = [Math.sin(phi) * Math.cos(theta), Math.sin(phi) * Math.sin(theta), Math.cos(phi)];
      b.points.push(centre[0] + n[0] * radius, centre[1] + n[1] * radius, centre[2] + n[2] * radius);
      b.normals.push(n[0], n[1], n[2]);
    }
  }
  const denseRings = Math.round(Math.PI * radius / 0.015);
  for (let a = 1; a < denseRings; a += 1) {
    const phi = Math.PI * a / denseRings; const ringRadius = Math.sin(phi) * radius;
    const around = Math.max(6, Math.round(2 * Math.PI * ringRadius / 0.015));
    for (let c = 0; c < around; c += 1) {
      const theta = 2 * Math.PI * c / around;
      b.truth.push(centre[0] + Math.sin(phi) * Math.cos(theta) * radius, centre[1] + Math.sin(phi) * Math.sin(theta) * radius, centre[2] + Math.cos(phi) * radius);
    }
  }
}
const finish = (name: string, b: Builder, voids: SyntheticSurfaceScene['voids'] = []): SyntheticSurfaceScene => ({
  name, points: Float32Array.from(b.points), normals: Float32Array.from(b.normals), truth: Float32Array.from(b.truth), voids,
});

export function planeScene(step = 0.05): SyntheticSurfaceScene {
  const b = builder(); rectangle(b, [0, 0, 0], [2, 0, 0], [0, 2, 0], [0, 0, 1], step); return finish('plane', b);
}
/** Floor and two walls meeting in an inside corner. */
export function cornerScene(step = 0.05): SyntheticSurfaceScene {
  const b = builder();
  rectangle(b, [0, 0, 0], [2, 0, 0], [0, 2, 0], [0, 0, 1], step);
  rectangle(b, [0, 0, 0], [0, 2, 0], [0, 0, 1.2], [1, 0, 0], step);
  rectangle(b, [0, 0, 0], [2, 0, 0], [0, 0, 1.2], [0, 1, 0], step);
  return finish('corner', b);
}
/** A free-standing cube seen from outside (five faces; the bottom rests on the floor). */
export function cubeScene(step = 0.05): SyntheticSurfaceScene {
  const b = builder(); const s = 0.8;
  rectangle(b, [0, 0, s], [s, 0, 0], [0, s, 0], [0, 0, 1], step);
  rectangle(b, [0, 0, 0], [s, 0, 0], [0, 0, s], [0, -1, 0], step);
  rectangle(b, [0, s, 0], [s, 0, 0], [0, 0, s], [0, 1, 0], step);
  rectangle(b, [0, 0, 0], [0, s, 0], [0, 0, s], [-1, 0, 0], step);
  rectangle(b, [s, 0, 0], [0, s, 0], [0, 0, s], [1, 0, 0], step);
  return finish('cube', b);
}
export function sphereScene(step = 0.05): SyntheticSurfaceScene {
  const b = builder(); sphere(b, [0, 0, 0.5], 0.5, step); return finish('sphere', b);
}
/** Two thin boards 15 cm apart, each seen from its outer side: the gap between them is not a surface. */
export function parallelWallsScene(step = 0.05, gap = 0.15): SyntheticSurfaceScene {
  const b = builder();
  rectangle(b, [0, 0, 0], [2, 0, 0], [0, 0, 1.2], [0, -1, 0], step);
  rectangle(b, [0, gap, 0], [2, 0, 0], [0, 0, 1.2], [0, 1, 0], step);
  return finish('parallel walls', b, [{ centre: [1, gap / 2, 0.6], radius: gap / 2 - 0.02 }]);
}
/** A wall 3 m wide with a 0.9 x 2.0 m doorway. */
export function doorwayScene(step = 0.05): SyntheticSurfaceScene {
  const b = builder();
  rectangle(b, [0, 0, 0], [3, 0, 0], [0, 0, 2.4], [0, -1, 0], step, 0.02, (s, t) => s > 1.05 && s < 1.95 && t < 2.0);
  return finish('doorway', b, [{ centre: [1.5, 0, 1.0], radius: 0.28 }]);
}
/** Two spheres with 15 cm between their surfaces. */
export function twoObjectsScene(step = 0.05): SyntheticSurfaceScene {
  const b = builder(); sphere(b, [0, 0, 0.4], 0.4, step); sphere(b, [0.95, 0, 0.4], 0.4, step);
  return finish('two objects', b, [{ centre: [0.475, 0, 0.4], radius: 0.05 }]);
}
/** Noisy measurements: 4 mm Gaussian noise and 2% outliers 5 to 15 cm off the surface. */
export function noisyScene(base: SyntheticSurfaceScene = cornerScene(), seed = 7): SyntheticSurfaceScene {
  const next = random(seed);
  const points = Float32Array.from(base.points); const count = points.length / 3;
  for (let i = 0; i < count; i += 1) {
    const outlier = next() < 0.02;
    const nx = base.normals[i * 3]; const ny = base.normals[i * 3 + 1]; const nz = base.normals[i * 3 + 2];
    const along = outlier ? (0.05 + 0.1 * next()) * (next() < 0.5 ? 1 : -1) : gaussian(next) * 0.004;
    points[i * 3] += nx * along; points[i * 3 + 1] += ny * along; points[i * 3 + 2] += nz * along;
  }
  return { ...base, name: `${base.name} (noisy)`, points };
}
/** A plane whose left half is sampled at 3 cm and right half at 9 cm. */
export function mixedDensityScene(): SyntheticSurfaceScene {
  const b = builder();
  rectangle(b, [0, 0, 0], [1, 0, 0], [0, 2, 0], [0, 0, 1], 0.03);
  rectangle(b, [1.09, 0, 0], [1, 0, 0], [0, 2, 0], [0, 0, 1], 0.09);
  return finish('mixed density', b);
}
/** A thin board (2 cm) measured from BOTH sides: two layers with opposite normals that must stay two layers, not cancel each other out. */
export function thinBoardScene(step = 0.05): SyntheticSurfaceScene {
  const b = builder();
  rectangle(b, [0, 0, 0.01], [1.6, 0, 0], [0, 1.6, 0], [0, 0, 1], step);
  rectangle(b, [0, 0, -0.01], [1.6, 0, 0], [0, 1.6, 0], [0, 0, -1], step);
  return finish('thin board both sides', b);
}
export const ALL_SCENES: ReadonlyArray<() => SyntheticSurfaceScene> = [planeScene, cornerScene, cubeScene, sphereScene, parallelWallsScene, doorwayScene, twoObjectsScene, () => noisyScene(), mixedDensityScene, thinBoardScene];
