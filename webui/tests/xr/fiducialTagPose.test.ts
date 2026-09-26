import { describe, expect, it } from 'vitest';
import { estimateTagPose } from '../../src/xr/fiducial/tagPose';
import {
  projectToImage,
  projectionMatrix,
  worldFromView,
  worldToProject,
  type CameraIntrinsics,
  type CameraPose,
  type Vec3,
} from './support/syntheticDepthScene';

const intrinsics: CameraIntrinsics = { width: 1280, height: 720, fovYDeg: 60 };
const camera: CameraPose = { eye: [0, 0, 0], target: [0, 1, 0] };
const projection = projectionMatrix(intrinsics);
const cameraToScan = worldFromView(camera);
const TAG_SIZE_M = 0.032;

function rotationX(degrees: number): number[] {
  const angle = degrees * Math.PI / 180;
  const c = Math.cos(angle); const s = Math.sin(angle);
  return [1, 0, 0, 0, c, -s, 0, s, c];
}

function transform(rotation: readonly number[], point: Vec3, translation: Vec3): Vec3 {
  return [
    rotation[0] * point[0] + rotation[1] * point[1] + rotation[2] * point[2] + translation[0],
    rotation[3] * point[0] + rotation[4] * point[1] + rotation[5] * point[2] + translation[1],
    rotation[6] * point[0] + rotation[7] * point[1] + rotation[8] * point[2] + translation[2],
  ];
}

function cornersAt(distanceM: number, tiltDeg: number, translationX = 0.04, translationY = -0.025): [number, number][] {
  const rotation = rotationX(tiltDeg); const half = TAG_SIZE_M / 2;
  const local: Vec3[] = [[-half, -half, 0], [half, -half, 0], [half, half, 0], [-half, half, 0]];
  return local.map((point) => {
    const scan = transform(rotation, point, [translationX, translationY, -distanceM]);
    const image = projectToImage(camera, intrinsics, worldToProject(scan));
    if (image === null) throw new Error('Fixture point is behind its synthetic camera.');
    return image;
  }) as [number, number][];
}

function rotationErrorDeg(a: readonly number[], b: readonly number[]): number {
  let trace = 0;
  for (let row = 0; row < 3; row += 1) for (let column = 0; column < 3; column += 1) {
    trace += a[row * 3 + column] * b[row * 3 + column];
  }
  return Math.acos(Math.max(-1, Math.min(1, (trace - 1) / 2))) * 180 / Math.PI;
}

function estimate(corners: readonly [number, number][], tagSizeM = TAG_SIZE_M, minDepthM?: number) {
  return estimateTagPose({ corners: corners as never, projectionMatrix: projection, worldFromView: cameraToScan, tagSizeM, minDepthM });
}

describe('AprilTag planar pose', () => {
  it.each([0.3, 1, 3])('recovers pose at %s m and tilts through 70 degrees', (distance) => {
    for (const tilt of [0, 30, 60, 70]) {
      const result = estimate(cornersAt(distance, tilt));
      expect(result.ok, `distance=${distance}, tilt=${tilt}, reason=${result.ok ? '' : result.reason}`).toBe(true);
      if (!result.ok) continue;
      expect(Math.hypot(result.positionScan[0] - 0.04, result.positionScan[1] + 0.025, result.positionScan[2] + distance)).toBeLessThanOrEqual(0.002);
      expect(rotationErrorDeg(result.rotationScan, rotationX(tilt))).toBeLessThanOrEqual(1);
      expect(result.distanceM).toBeCloseTo(Math.hypot(0.04, -0.025, distance), 3);
      expect(result.reprojectionErrorNorm).toBeLessThan(1e-8);
    }
  });

  it('keeps a bounded pose error under deterministic plus-or-minus 0.5 px corner noise', () => {
    const signs = [[0.5, -0.5], [-0.5, 0.5], [0.5, 0.5], [-0.5, -0.5]];
    const noisy = cornersAt(1, 30).map(([u, v], index) => [u + signs[index][0] / intrinsics.width, v + signs[index][1] / intrinsics.height]) as [number, number][];
    const result = estimate(noisy);
    expect(result.ok).toBe(true);
    if (!result.ok) return;
    // This marker spans about 20 px at 1 m; subpixel corner noise is bounded to 60 mm and 8 degrees.
    expect(Math.hypot(result.positionScan[0] - 0.04, result.positionScan[1] + 0.025, result.positionScan[2] + 1)).toBeLessThanOrEqual(0.06);
    expect(rotationErrorDeg(result.rotationScan, rotationX(30))).toBeLessThanOrEqual(8);
  });

  it('rejects tags smaller than the minimum inferred depth', () => {
    const largeImageTag = cornersAt(0.5, 0);
    expect(estimate(largeImageTag, 0.002)).toEqual({ ok: false, reason: 'too_close_or_behind' });
  });

  it('accepts just beyond and rejects just inside the minimum depth boundary', () => {
    const corners = cornersAt(1, 0, 0, 0);
    const minDepthM = 0.05;
    const epsilonM = 0.001;
    const justBeyond = estimate(corners, TAG_SIZE_M * (minDepthM + epsilonM), minDepthM);
    const justInside = estimate(corners, TAG_SIZE_M * (minDepthM - epsilonM), minDepthM);
    expect(justBeyond.ok).toBe(true);
    expect(justInside).toEqual({ ok: false, reason: 'too_close_or_behind' });
  });

  it('uses a custom minimum depth', () => {
    const corners = cornersAt(1, 0, 0, 0);
    const scaledTagSizeM = TAG_SIZE_M * 0.075;
    expect(estimate(corners, scaledTagSizeM).ok).toBe(true);
    expect(estimate(corners, scaledTagSizeM, 0.08)).toEqual({ ok: false, reason: 'too_close_or_behind' });
  });

  it('rejects malformed, degenerate, reversed, and singular inputs', () => {
    const good = cornersAt(1, 0);
    const run = (corners: [number, number][], tagSizeM = TAG_SIZE_M, matrix: ArrayLike<number> = projection, minDepthM?: number) =>
      estimateTagPose({ corners: corners as never, projectionMatrix: matrix, worldFromView: cameraToScan, tagSizeM, minDepthM });
    expect(run([[Number.NaN, 0], ...good.slice(1)]).reason).toBe('non_finite_input');
    expect(run(good, 0).reason).toBe('invalid_tag_size');
    expect(run([[0.2, 0.2], [0.4, 0.4], [0.6, 0.6], [0.8, 0.8]]).reason).toBe('degenerate_corners');
    expect(run([[0.3, 0.3], [0.7, 0.7], [0.3, 0.7], [0.7, 0.3]]).reason).toBe('self_intersecting');
    expect(run([good[0], good[3], good[2], good[1]]).reason).toBe('wrong_winding');
    expect(run(good, TAG_SIZE_M, projection, -0.01).reason).toBe('invalid_min_depth');
    expect(run(good, TAG_SIZE_M, new Array(16).fill(0)).reason).toBe('singular_projection');
  });
});
