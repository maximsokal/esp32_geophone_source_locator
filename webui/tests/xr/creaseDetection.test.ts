import { describe, expect, it } from 'vitest';

import { DepthPointCloudAccumulator } from '../../src/xr/depthPointCloudV2';
import { classifyDepthNeighbourhood, creaseResidualMeters, type DepthStencilGeometry } from '../../src/xr/surfelEvidence';
import {
  ARCORE_NOISE, DEFAULT_INTRINSICS, boxCornerScene, distanceToCrease, orbitPoses, polygonAreaSplit,
  projectToImage, projectionMatrix, renderDepth, ridgeScene, scanScene, type SyntheticScene,
} from './support/syntheticDepthScene';

/*
 * MAX-252: creases (continuous depth, slope change) must be detected; planes
 * of any slope must not. Every stencil below is computed from view-space
 * geometry, never typed in.
 */

const DEG = Math.PI / 180;
/** View-space tangent between adjacent depth pixels for the harness camera. */
const PIXEL_TANGENT = 2 / (projectionMatrix(DEFAULT_INTRINSICS)[0] * DEFAULT_INTRINSICS.width);

/**
 * Specified sensor noise (1 sigma) the detector must cope with; stated here
 * independently of the implementation: ~1 mm floor + 1.5 mm * d^2.
 */
function specSigma(depth: number): number { return 0.001 + 0.0015 * depth * depth; }

/** Depth along the ray with tangent t of a plane through (0, c) whose normal is tilted by theta from the view axis. */
function planeDepth(c: number, thetaDeg: number, t: number): number {
  const theta = thetaDeg * DEG;
  return (c * Math.cos(theta)) / (t * Math.sin(theta) + Math.cos(theta));
}

interface Stencil { center: number; left: number; right: number; up: number; down: number; geometry: DepthStencilGeometry }

/** u-axis profile from two half-planes meeting at the centre ray; v axis flat. */
function stencil(c: number, leftThetaDeg: number, rightThetaDeg: number, leftPx: number, rightPx: number): Stencil {
  const left = planeDepth(c, leftThetaDeg, -leftPx * PIXEL_TANGENT);
  const right = planeDepth(c, rightThetaDeg, rightPx * PIXEL_TANGENT);
  return {
    center: c, left, right, up: c, down: c,
    geometry: { left: leftPx * PIXEL_TANGENT, right: rightPx * PIXEL_TANGENT, up: PIXEL_TANGENT, down: PIXEL_TANGENT },
  };
}

function classify(s: Stencil) {
  return classifyDepthNeighbourhood(s.center, s.left, s.right, s.up, s.down, s.geometry);
}

// Nearest-pixel reads of a 1.5 px stencil land 1 or 2 pixels away.
const OFFSETS: ReadonlyArray<readonly [number, number]> = [[1, 1], [1, 2], [2, 1], [2, 2]];

describe('classifyDepthNeighbourhood crease detection', () => {
  it('creaseResidualMeters is exactly zero for any plane, however wide or uneven the stencil', () => {
    // Inverse depth is affine in the view tangent for a plane; a depth-linear chord is not.
    for (const tilt of [0, 40, 70, 80]) for (const [l, r] of [[10, 10], [5, 40], [30, 7]]) {
      const c = 2.0;
      const residual = creaseResidualMeters(c, l * PIXEL_TANGENT, planeDepth(c, tilt, -l * PIXEL_TANGENT), r * PIXEL_TANGENT, planeDepth(c, tilt, r * PIXEL_TANGENT));
      expect(Math.abs(residual!), `tilt ${tilt} ${l}/${r}`).toBeLessThan(1e-9);
    }
    // Unusable stencils are reported as such, not as zero curvature.
    expect(creaseResidualMeters(1, 0, 1, PIXEL_TANGENT, 1)).toBeNull();
    expect(creaseResidualMeters(1, PIXEL_TANGENT, null, PIXEL_TANGENT, 1)).toBeNull();
  });

  it('never reports a crease on a plane, whatever its slope, range or stencil asymmetry', () => {
    for (const tilt of [0, 30, 60, 75, -60]) for (const depth of [1.0, 1.5, 2.5]) for (const [l, r] of OFFSETS) {
      const quality = classify(stencil(depth, tilt, tilt, l, r));
      expect(quality.crease, `tilt ${tilt} depth ${depth} offsets ${l}/${r}`).toBe(false);
      // First-difference discontinuity detection is unchanged and knowingly
      // ambiguous for steep far planes; only gentle planes are asserted here.
      if (Math.abs(tilt) <= 30) expect(quality.edge).toBe(false);
    }
  });

  it('detects convex and concave 90 degree creases on the correct axis, with the correct sign', () => {
    for (const depth of [1.0, 1.5]) for (const [l, r] of OFFSETS) {
      const convex = classify(stencil(depth, 45, -45, l, r)); // both faces recede: ridge toward the camera
      expect(convex.crease, `convex ${depth} ${l}/${r}`).toBe(true);
      expect(convex.edge).toBe(false);
      expect(convex.creaseAxis).toBe('u');
      expect(convex.creaseResidualMeters).toBeGreaterThan(0);
      const concave = classify(stencil(depth, -45, 45, l, r)); // valley
      expect(concave.crease, `concave ${depth} ${l}/${r}`).toBe(true);
      expect(concave.creaseResidualMeters).toBeLessThan(0);
      // A crease sample is a real surface point but must not score like a flat interior one.
      expect(convex.score).toBeLessThanOrEqual(classify(stencil(depth, 0, 0, l, r)).score);
    }
    // Same crease along v.
    const c = 1.2;
    const up = planeDepth(c, 45, -PIXEL_TANGENT);
    const down = planeDepth(c, -45, PIXEL_TANGENT);
    const vertical = classifyDepthNeighbourhood(c, c, c, up, down, { left: PIXEL_TANGENT, right: PIXEL_TANGENT, up: PIXEL_TANGENT, down: PIXEL_TANGENT });
    expect(vertical.crease).toBe(true);
    expect(vertical.creaseAxis).toBe('v');
  });

  it('scales the crease decision with the range-dependent sensor noise', () => {
    // Symmetric stencil whose neighbours sit exactly `delta` behind the centre:
    // the chord deviation is then delta. Detect >= 4.5 sigma, ignore <= 2 sigma.
    for (const depth of [0.6, 1.0, 1.5, 2.5, 3.5]) {
      const geometry = { left: PIXEL_TANGENT, right: PIXEL_TANGENT, up: PIXEL_TANGENT, down: PIXEL_TANGENT };
      const strong = depth + 4.5 * specSigma(depth);
      const weak = depth + 2 * specSigma(depth);
      expect(classifyDepthNeighbourhood(depth, strong, strong, depth, depth, geometry).crease, `strong ${depth}`).toBe(true);
      expect(classifyDepthNeighbourhood(depth, weak, weak, depth, depth, geometry).crease, `weak ${depth}`).toBe(false);
    }
  });

  it('treats a depth step as a discontinuity, not as a crease', () => {
    for (const depth of [1.0, 2.0]) {
      const quality = classifyDepthNeighbourhood(depth, depth, depth + 0.2, depth, depth, { left: PIXEL_TANGENT, right: PIXEL_TANGENT, up: PIXEL_TANGENT, down: PIXEL_TANGENT });
      expect(quality.edge).toBe(true);
      expect(quality.crease).toBe(false);
    }
  });

  it('keeps the flat-surface false-positive rate at or below 1 % under the specified noise', () => {
    let seed = 0x2545f491;
    const gaussian = (): number => {
      seed = (Math.imul(seed, 1103515245) + 12345) >>> 0; const u1 = (seed + 0.5) / 4294967296;
      seed = (Math.imul(seed, 1103515245) + 12345) >>> 0; const u2 = (seed + 0.5) / 4294967296;
      return Math.sqrt(-2 * Math.log(u1)) * Math.cos(2 * Math.PI * u2);
    };
    for (const depth of [1.0, 2.0]) {
      let positives = 0;
      for (let i = 0; i < 1000; i += 1) {
        const s = stencil(depth, 30, 30, 1, 2);
        const n = (): number => gaussian() * specSigma(depth);
        if (classifyDepthNeighbourhood(s.center + n(), s.left + n(), s.right + n(), s.up + n(), s.down + n(), s.geometry).crease) positives += 1;
      }
      expect(positives, `depth ${depth}`).toBeLessThanOrEqual(10);
    }
  });
});

describe('crease detection on rendered multi-view depth', () => {
  it('flags pixels on a rendered 90 deg ridge and not on its faces', () => {
    const scene = ridgeScene(90);
    const pose = orbitPoses(scene.focus, 1.4, 55, 30, 30, 1)[0];
    const frame = renderDepth(scene, pose, 1, ARCORE_NOISE);
    const { width, height } = DEFAULT_INTRINSICS;
    const at = (column: number, row: number): number | null => {
      if (column < 0 || row < 0 || column >= width || row >= height) return null;
      const value = frame.buffer[row * width + column];
      return Number.isFinite(value) ? value : null;
    };
    const geometry = { left: PIXEL_TANGENT, right: PIXEL_TANGENT, up: 2 / (projectionMatrix(DEFAULT_INTRINSICS)[5] * height), down: 2 / (projectionMatrix(DEFAULT_INTRINSICS)[5] * height) };
    // Pixel distance of each pixel centre from the projected crease segment.
    const a = projectToImage(pose, DEFAULT_INTRINSICS, scene.creases[0].start)!;
    const b = projectToImage(pose, DEFAULT_INTRINSICS, scene.creases[0].end)!;
    const pixelDistance = (column: number, row: number): number => {
      const px = [(column + 0.5) / width * width, (row + 0.5) / height * height];
      const pa = [a[0] * width, a[1] * height]; const pb = [b[0] * width, b[1] * height];
      const abx = pb[0] - pa[0], aby = pb[1] - pa[1];
      const t = Math.max(0, Math.min(1, ((px[0] - pa[0]) * abx + (px[1] - pa[1]) * aby) / (abx * abx + aby * aby)));
      return Math.hypot(px[0] - pa[0] - t * abx, px[1] - pa[1] - t * aby);
    };
    let onCrease = 0; let onCreaseFlagged = 0; let onFace = 0; let onFaceFlagged = 0;
    for (let row = 2; row < height - 2; row += 1) for (let column = 2; column < width - 2; column += 1) {
      const c = at(column, row); const l = at(column - 1, row); const r = at(column + 1, row); const u = at(column, row - 1); const d = at(column, row + 1);
      if (c === null || l === null || r === null || u === null || d === null) continue;
      const distance = pixelDistance(column, row);
      const quality = classifyDepthNeighbourhood(c, l, r, u, d, geometry);
      if (distance < 0.5) { onCrease += 1; if (quality.crease) onCreaseFlagged += 1; }
      if (distance > 4) { onFace += 1; if (quality.crease) onFaceFlagged += 1; }
    }
    expect(onCrease).toBeGreaterThan(20);
    expect(onFace).toBeGreaterThan(200);
    expect(onCreaseFlagged / onCrease).toBeGreaterThanOrEqual(0.5);
    expect(onFaceFlagged / onFace).toBeLessThanOrEqual(0.01);
  });

  it('regression: promoted density in the 1-voxel crease band stays >= 0.8x the face density', () => {
    // Measured on main before MAX-252: ratios 0.96-1.33, so no densification
    // was added; this guards against future retention changes thinning creases.
    const voxel = 0.04;
    const cases: Array<[SyntheticScene, number, number, number]> = [[ridgeScene(90), 55, 0, 315], [boxCornerScene(), 35, 10, 80]];
    for (const [scene, elevation, from, to] of cases) {
      const accumulator = new DepthPointCloudAccumulator(voxel);
      scanScene(accumulator, scene, orbitPoses(scene.focus, 1.4, elevation, from, to, 10), { noise: ARCORE_NOISE });
      const snapshot = accumulator.snapshot(10, 10);
      let near = 0; let face = 0;
      for (let i = 0; i < snapshot.pointCount; i += 1) {
        const d = distanceToCrease(scene, [snapshot.positions[i * 3], snapshot.positions[i * 3 + 1], snapshot.positions[i * 3 + 2]]);
        if (d < voxel) near += 1; else if (d >= 2 * voxel && d < 5 * voxel) face += 1;
      }
      const nearArea = polygonAreaSplit(scene, voxel).nearCrease;
      const faceArea = polygonAreaSplit(scene, 5 * voxel).nearCrease - polygonAreaSplit(scene, 2 * voxel).nearCrease;
      expect(near / nearArea, scene.name).toBeGreaterThanOrEqual(0.8 * (face / faceArea));
    }
  });
});
