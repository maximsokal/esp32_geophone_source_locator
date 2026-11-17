import { describe, expect, it } from 'vitest';

import { AR } from '../../src/xr/fiducial/jsAruco2';
import { detectAprilTags } from '../../src/xr/fiducial/tagDetector';
import { fuseTagObservations } from '../../src/xr/fiducial/tagObservationFusion';
import { TagObservationStore } from '../../src/xr/fiducial/tagObservationStore';
import { tagChipStatuses } from '../../src/xr/fiducial/tagModeState';
import { estimateTagPose } from '../../src/xr/fiducial/tagPose';
import { TAG_CAPTURE_HEIGHT, TAG_CAPTURE_WIDTH } from '../../src/xr/webxrRuntime';
import { defaultRegistrationTagSizes } from '../../src/xr/fiducial/registrationArtifact';
import {
  projectToImage, projectionMatrix, worldFromView, worldToProject,
  type CameraIntrinsics, type CameraPose, type Vec3,
} from './support/syntheticDepthScene';

/*
 * Board 2026-11-17: pointing the phone camera at a sensor showed no confirmation. Every stage had its own test (detector, pose, store,
 * fusion, chips) but nothing ran a picture all the way to the chip the operator reads. This does: a rendered 960x540 camera frame per
 * viewpoint -> detectAprilTags -> estimateTagPose -> TagObservationStore -> fuseTagObservations -> tagChipStatuses, the same calls and
 * thresholds XrRuntimePanel makes (5 observations, 3 independent views).
 */

// The size the phone actually captures for tag detection, so a change to it re-runs this proof.
const WIDTH = TAG_CAPTURE_WIDTH;
const HEIGHT = TAG_CAPTURE_HEIGHT;
const intrinsics: CameraIntrinsics = { width: WIDTH, height: HEIGHT, fovYDeg: 45 };
const projection = projectionMatrix(intrinsics);
const TAG_SIZE_M = defaultRegistrationTagSizes()['1'];

interface RenderOptions { noise?: number; blurPasses?: number }

function solveCell(point: readonly [number, number], quad: readonly (readonly [number, number])[]): [number, number] | null {
  const [p00, p10, p11, p01] = quad;
  const bx = p00[0] - p10[0] + p11[0] - p01[0]; const by = p00[1] - p10[1] + p11[1] - p01[1];
  let u = 0.5; let v = 0.5;
  for (let i = 0; i < 6; i += 1) {
    const x = p00[0] + u * (p10[0] - p00[0]) + v * (p01[0] - p00[0]) + u * v * bx;
    const y = p00[1] + u * (p10[1] - p00[1]) + v * (p01[1] - p00[1]) + u * v * by;
    const j00 = p10[0] - p00[0] + v * bx; const j01 = p01[0] - p00[0] + u * bx;
    const j10 = p10[1] - p00[1] + v * by; const j11 = p01[1] - p00[1] + u * by;
    const det = j00 * j11 - j01 * j10;
    if (Math.abs(det) < 1e-8) return null;
    u -= (j11 * (x - point[0]) - j01 * (y - point[1])) / det;
    v -= (-j10 * (x - point[0]) + j00 * (y - point[1])) / det;
  }
  return [u, v];
}

/** The tag is fixed in the scene (centre `centre`, facing -Y toward the origin side); only the camera moves. */
function renderFrame(tagId: number, camera: CameraPose, centre: Vec3, options: RenderOptions = {}): Uint8Array {
  const rgba = new Uint8Array(WIDTH * HEIGHT * 4);
  rgba.fill(255);
  const code = new AR.Dictionary('APRILTAG_36h11')._hex2bin(AR.DICTIONARIES.APRILTAG_36h11.codeList[tagId], 36);
  const h = TAG_SIZE_M / 2;
  // Scene-space corners (the layout detectAprilTags' own test uses): x right, y up in the tag plane, z the depth axis.
  const scene: Vec3[] = [[-h, -h, 0], [h, -h, 0], [h, h, 0], [-h, h, 0]].map(([x, y, z]) => [
    x + centre[0], y + centre[1], z + centre[2],
  ] as unknown as Vec3);
  const px = scene.map((corner) => {
    const uv = projectToImage(camera, intrinsics, worldToProject(corner));
    if (uv === null) throw new Error('tag corner behind the camera');
    return [uv[0] * (WIDTH - 1), uv[1] * (HEIGHT - 1)] as const;
  });
  const display = [px[3], px[2], px[1], px[0]] as const;
  const minX = Math.max(0, Math.floor(Math.min(...display.map((p) => p[0])) - 2));
  const maxX = Math.min(WIDTH - 1, Math.ceil(Math.max(...display.map((p) => p[0])) + 2));
  const minY = Math.max(0, Math.floor(Math.min(...display.map((p) => p[1])) - 2));
  const maxY = Math.min(HEIGHT - 1, Math.ceil(Math.max(...display.map((p) => p[1])) + 2));
  for (let y = minY; y <= maxY; y += 1) for (let x = minX; x <= maxX; x += 1) {
    let black = 0;
    for (const sy of [0.25, 0.75]) for (const sx of [0.25, 0.75]) {
      const uv = solveCell([x + sx, y + sy], display);
      if (uv === null || uv[0] < 0 || uv[0] > 1 || uv[1] < 0 || uv[1] > 1) continue;
      const column = Math.min(7, Math.floor(uv[0] * 8)); const row = Math.min(7, Math.floor(uv[1] * 8));
      const white = column > 0 && column < 7 && row > 0 && row < 7 && code[(row - 1) * 6 + column - 1] === '1';
      if (!white) black += 1;
    }
    const gray = 255 - black * 255 / 4; const offset = (y * WIDTH + x) * 4;
    rgba[offset] = gray; rgba[offset + 1] = gray; rgba[offset + 2] = gray;
  }
  for (let i = 3; i < rgba.length; i += 4) rgba[i] = 255;
  let seed = 12345;
  for (let pass = 0; pass < (options.blurPasses ?? 0); pass += 1) {
    const copy = rgba.slice();
    for (let y = 1; y < HEIGHT - 1; y += 1) for (let x = 1; x < WIDTH - 1; x += 1) {
      let sum = 0;
      for (let dy = -1; dy <= 1; dy += 1) for (let dx = -1; dx <= 1; dx += 1) sum += copy[((y + dy) * WIDTH + x + dx) * 4];
      const value = sum / 9; const o = (y * WIDTH + x) * 4;
      rgba[o] = value; rgba[o + 1] = value; rgba[o + 2] = value;
    }
  }
  if ((options.noise ?? 0) > 0) {
    for (let i = 0; i < rgba.length; i += 4) {
      seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
      const delta = ((seed >>> 8) / 0xffffff - 0.5) * 2 * (options.noise ?? 0);
      const value = Math.max(0, Math.min(255, rgba[i] + delta));
      rgba[i] = value; rgba[i + 1] = value; rgba[i + 2] = value;
    }
  }
  return rgba;
}

/** Cameras looking at `centre` from azimuths (degrees) around it, `radius` metres away, in the scene's project space. */
function poses(centre: Vec3, azimuths: readonly number[], radius: number): CameraPose[] {
  const target: Vec3 = [centre[0], -centre[2], centre[1]];
  return azimuths.map((degrees) => {
    const a = degrees * Math.PI / 180;
    return { eye: [target[0] + radius * Math.sin(a), target[1] - radius * Math.cos(a), target[2]] as Vec3, target };
  });
}

function observe(tagId: number, camera: CameraPose, centre: Vec3, store: TagObservationStore, options?: RenderOptions): number {
  const rgba = renderFrame(tagId, camera, centre, options);
  const detections = detectAprilTags(WIDTH, HEIGHT, rgba);
  const worldFrom = worldFromView(camera);
  let stored = 0;
  for (const detection of detections.filter((item) => item.tagId === tagId)) {
    const pose = estimateTagPose({
      corners: detection.corners, projectionMatrix: projection, worldFromView: worldFrom, tagSizeM: TAG_SIZE_M,
      capturedAtMs: 1000 + stored, cameraPosition: [worldFrom[12], worldFrom[13], worldFrom[14]],
      cameraForward: [-worldFrom[8], -worldFrom[9], -worldFrom[10]],
    });
    if (!pose.ok) continue;
    store.insert({
      tagId: detection.tagId, positionScan: pose.positionScan, capturedAtMs: pose.capturedAtMs,
      cameraPosition: pose.cameraPosition, cameraForward: pose.cameraForward,
    });
    stored += 1;
  }
  return stored;
}

const CENTRE: Vec3 = [0.1, 0.0, -0.9];
const SPREAD = [-45, -30, -15, 0, 15, 30, 45];

describe('a camera frame to the chip the operator reads', () => {
  it.each([0, 1, 2, 3, 4])('confirms tag %i from seven viewpoints, at the true position', (tagId) => {
    const store = new TagObservationStore();
    for (const camera of poses(CENTRE, SPREAD, 0.6)) expect(observe(tagId, camera, CENTRE, store)).toBe(1);
    const fusion = fuseTagObservations(store.getObservations(), { minObservations: 5, minIndependentViews: 3 });
    const tag = fusion.byTagId[tagId as 0 | 1 | 2 | 3 | 4]!;
    expect(tag.confirmed, JSON.stringify(tag)).toBe(true);
    const chip = tagChipStatuses(fusion, 'en').find((item) => item.tagId === tagId)!;
    expect(chip.status).toBe('confirmed');
    expect(chip.text).toContain('confirmed');
    const truth = [CENTRE[0], CENTRE[1], CENTRE[2]];
    expect(Math.hypot(...tag.positionScan.map((value, axis) => value - truth[axis]))).toBeLessThan(0.01);
  });

  it('holding the camera still shows the tag as seen with its views, never as confirmed (what a short pointing gives)', () => {
    const store = new TagObservationStore();
    const [camera] = poses(CENTRE, [0], 0.6);
    for (let i = 0; i < 8; i += 1) expect(observe(1, camera, CENTRE, store)).toBe(1);
    const fusion = fuseTagObservations(store.getObservations(), { minObservations: 5, minIndependentViews: 3 });
    expect(fusion.byTagId[1]!.confirmed).toBe(false);
    const chip = tagChipStatuses(fusion, 'en').find((item) => item.tagId === 1)!;
    expect(chip.status).toBe('seen');
    expect(chip.text).toMatch(/views 1\//);
  });

  it('a tag nobody pointed at stays not seen', () => {
    const fusion = fuseTagObservations(new TagObservationStore().getObservations(), { minObservations: 5, minIndependentViews: 3 });
    for (const chip of tagChipStatuses(fusion, 'en')) expect(chip.status).toBe('not-seen');
  });

  /*
   * Measured with the sweep this file grew from (7 views, tag 2, 32 mm tag, 1280x720 frame, 3x3 box blur = a soft hand-held image;
   * 5 of 7 views are needed to confirm). At 960x540 a soft image lost the tag beyond ~0.6 m (1 m: 0-2 of 7 views). At 1280x720:
   * 0.4-0.6 m detects in every view even with +-24 levels of noise; a soft image keeps 5-7 of 7 up to 1 m (4 with heavy noise at
   * 0.8-1 m); a sharp image keeps 6-7 of 7 up to 1.2 m and 5-6 at 1.5 m; 2 m is out of reach either way. The reach is set by the
   * detector's 12 px edge minimum, so it grows with the capture width; a larger tag would move it again.
   */
  it('reaches 1 m on a soft image and 1.2 m on a sharp one (960x540 reached 0.6 m and 1 m)', () => {
    for (const [radius, options] of [[1, { blurPasses: 1 }], [1.2, {}]] as const) {
      const store = new TagObservationStore();
      for (const camera of poses(CENTRE, SPREAD, radius)) observe(2, camera, CENTRE, store, options);
      const fusion = fuseTagObservations(store.getObservations(), { minObservations: 5, minIndependentViews: 3 });
      expect(fusion.byTagId[2]?.confirmed, `radius ${radius}: ${JSON.stringify(fusion.byTagId[2])}`).toBe(true);
    }
  });

  it('survives a noisy, slightly soft camera image at 0.4 m and 0.6 m', () => {
    for (const radius of [0.4, 0.6]) {
      const store = new TagObservationStore();
      for (const camera of poses(CENTRE, SPREAD, radius)) observe(2, camera, CENTRE, store, { noise: 24, blurPasses: 1 });
      const fusion = fuseTagObservations(store.getObservations(), { minObservations: 5, minIndependentViews: 3 });
      expect(fusion.byTagId[2]?.confirmed, `radius ${radius}: ${JSON.stringify(fusion.byTagId[2])}`).toBe(true);
    }
  });
});
