import { describe, expect, it } from 'vitest';
import { projectProjectPointToNormalized } from '../../src/xr/projectionMath';
import { liveTagOverlayQuads, LiveTagTracker, tagWorldCorners, xrToProject, type LiveTagInput, type Quad2, type Vec3 } from '../../src/xr/fiducial/liveTagTracker';
import { estimateTagPose } from '../../src/xr/fiducial/tagPose';
import { defaultRegistrationTagSizes } from '../../src/xr/fiducial/registrationArtifact';
import {
  projectToImage, projectionMatrix, worldFromView, worldToProject, type CameraIntrinsics, type CameraPose,
} from './support/syntheticDepthScene';

/* Geometry only (no rendering): corners are projected exactly, so every difference seen here is the tracker's own behaviour. */
const intrinsics: CameraIntrinsics = { width: 1280, height: 720, fovYDeg: 45 };
const projection = projectionMatrix(intrinsics);
const SIZE = defaultRegistrationTagSizes()['2'];
const CENTRE: Vec3 = [0.1, 0, -0.9];
const H = SIZE / 2;
/** True tag square in the XR world, order BL, BR, TR, TL, facing +Z. */
const TRUE_WORLD: Vec3[] = [[-H, -H, 0], [H, -H, 0], [H, H, 0], [-H, H, 0]].map(([x, y, z]) => [x + CENTRE[0], y + CENTRE[1], z + CENTRE[2]] as Vec3);

function cameraAt(azimuthDeg: number, radius = 0.6): CameraPose {
  const target = worldToProject(CENTRE);
  const a = azimuthDeg * Math.PI / 180;
  return { eye: [target[0] + radius * Math.sin(a), target[1] - radius * Math.cos(a), target[2]], target };
}

function corners2d(camera: CameraPose, jitterPx = 0, seed = 1): Quad2 {
  let s = seed;
  const rnd = () => { s = (Math.imul(s, 1664525) + 1013904223) >>> 0; return (s / 0xffffffff - 0.5) * 2; };
  return TRUE_WORLD.map((corner) => {
    const uv = projectToImage(camera, intrinsics, worldToProject(corner))!;
    return [uv[0] + (rnd() * jitterPx) / 1279, uv[1] + (rnd() * jitterPx) / 719] as const;
  }) as unknown as Quad2;
}

function input(camera: CameraPose, atMs: number, jitterPx = 0, seed = 1, tagId = 2): LiveTagInput {
  const corners = corners2d(camera, jitterPx, seed);
  const pose = estimateTagPose({ corners, projectionMatrix: projection, worldFromView: worldFromView(camera), tagSizeM: SIZE, capturedAtMs: atMs });
  return { tagId, corners, pose, sizeM: SIZE, capturedAtMs: atMs };
}

const projectWith = (camera: CameraPose) => (point: Vec3) => {
  const p = projectProjectPointToNormalized(point, projection, worldFromView(camera));
  return p === null ? null : { u: p.u, v: p.v };
};
const dist2 = (a: readonly number[], b: readonly number[]) => Math.hypot(a[0] - b[0], a[1] - b[1]);

describe('live tag tracker', () => {
  it('anchors the tag square in the world at the true corners, in tag-plane order', () => {
    const tracker = new LiveTagTracker();
    expect(tracker.ingest(input(cameraAt(20), 100))).toBe(true);
    const [state] = tracker.states();
    expect(state.status).toBe('POSE_ESTIMATED');
    expect(state.node).toBe('G2');
    state.cornersWorld!.forEach((corner, i) => expect(Math.hypot(corner[0] - TRUE_WORLD[i][0], corner[1] - TRUE_WORLD[i][1], corner[2] - TRUE_WORLD[i][2])).toBeLessThan(0.004));
    expect(tracker.ingest(input(cameraAt(20), 200))).toBe(false);
  });

  it('keeps the outline on the sensor when the camera moves (world-anchored), unlike the capture-time image position', () => {
    const tracker = new LiveTagTracker();
    tracker.ingest(input(cameraAt(-25), 100));
    // Move AND turn the phone so the sensor leaves the middle of the picture (an orbit around it would keep it centred).
    const orbit = cameraAt(25);
    const later: CameraPose = { eye: orbit.eye, target: [orbit.target[0] + 0.18, orbit.target[1], orbit.target[2] + 0.05] };
    const [quad] = liveTagOverlayQuads(tracker.states(), projectWith(later));
    expect(quad.style).toBe('anchored');
    TRUE_WORLD.forEach((corner, i) => {
      const expected = projectToImage(later, intrinsics, worldToProject(corner))!;
      expect(dist2(quad.points[i], expected)).toBeLessThan(0.004);
    });
    // The capture-time corners are far from where the sensor now is, so a 2D-only overlay would visibly drift.
    const captured = tracker.states()[0].corners2d;
    const drift = captured.reduce((sum, c, i) => sum + dist2(c, projectToImage(later, intrinsics, worldToProject(TRUE_WORLD[i]))!), 0) / 4;
    expect(drift).toBeGreaterThan(0.03);
  });

  it('smooths per-frame pose noise', () => {
    const tracker = new LiveTagTracker();
    const raw: number[] = []; const smooth: number[] = [];
    for (let i = 0; i < 40; i += 1) {
      const camera = cameraAt(15);
      const item = input(camera, i * 100, 0.8, i + 7);
      if (item.pose.ok) raw.push(item.pose.positionScan[0]);
      tracker.ingest(item);
      const world = tracker.states()[0].cornersWorld;
      if (i >= 5 && world !== null) smooth.push((world[0][0] + world[2][0]) / 2);
    }
    const std = (values: number[]) => { const m = values.reduce((a, b) => a + b, 0) / values.length; return Math.sqrt(values.reduce((a, b) => a + (b - m) ** 2, 0) / values.length); };
    expect(raw.length).toBeGreaterThan(30);
    expect(std(smooth)).toBeLessThan(std(raw.slice(5)) * 0.8);
  });

  it('rejects a one-off pose jump but re-anchors when the new position repeats', () => {
    const tracker = new LiveTagTracker();
    const camera = cameraAt(10);
    for (let i = 0; i < 4; i += 1) tracker.ingest(input(camera, i * 100));
    const anchored = tracker.states()[0].cornersWorld!.map((c) => [...c]);
    const moved = (atMs: number): LiveTagInput => {
      const base = input(camera, atMs);
      const pose = base.pose.ok ? { ...base.pose, positionScan: [base.pose.positionScan[0] + 0.3, base.pose.positionScan[1], base.pose.positionScan[2]] as [number, number, number] } : base.pose;
      return { ...base, pose };
    };
    tracker.ingest(moved(500));
    expect(tracker.states()[0].cornersWorld!.map((c) => [...c])).toEqual(anchored);
    expect(tracker.outliersRejected).toBe(1);
    tracker.ingest(input(camera, 600)); // a good frame in between resets the streak
    tracker.ingest(moved(700)); tracker.ingest(moved(800)); tracker.ingest(moved(900));
    expect(tracker.states()[0].cornersWorld![0][0]).toBeGreaterThan(anchored[0][0] + 0.2);
  });

  it('shows a decode without a trustworthy pose as provisional 2D at its image position', () => {
    const tracker = new LiveTagTracker();
    const camera = cameraAt(0);
    const base = input(camera, 100);
    tracker.ingest({ ...base, pose: { ok: false, reason: 'degenerate_corners' } });
    expect(tracker.states()[0].status).toBe('DETECTED_2D');
    const [quad] = liveTagOverlayQuads(tracker.states(), projectWith(cameraAt(40)));
    expect(quad.style).toBe('provisional');
    quad.points.forEach((p, i) => expect(dist2(p, base.corners[i])).toBeLessThan(1e-9));
    const bad = base.pose.ok ? { ...base.pose, reprojectionErrorNorm: 0.2 } : base.pose;
    const t2 = new LiveTagTracker();
    t2.ingest({ ...base, pose: bad });
    expect(t2.states()[0].status).toBe('DETECTED_2D');
    expect(t2.states()[0].cornersWorld).toBeNull();
    const far = base.pose.ok ? { ...base.pose, distanceM: 9 } : base.pose;
    const t3 = new LiveTagTracker();
    t3.ingest({ ...base, pose: far });
    expect(t3.states()[0].cornersWorld).toBeNull();
  });

  it('marks a silent tag LOST (keeping its anchored square, drawn dimmed) and recovers it when decoded again', () => {
    const tracker = new LiveTagTracker({ staleMs: 1000 });
    tracker.ingest(input(cameraAt(5), 1000));
    expect(tracker.tick(1900)).toEqual([]);
    expect(tracker.tick(2100)).toEqual([2]);
    expect(tracker.tick(2200)).toEqual([]); // reported once
    expect(tracker.states()[0].status).toBe('LOST');
    expect(liveTagOverlayQuads(tracker.states(), projectWith(cameraAt(5)))[0].style).toBe('lost');
    tracker.ingest(input(cameraAt(5), 3000));
    expect(tracker.states()[0].status).toBe('POSE_ESTIMATED');
  });

  it('reflects the multi-view fusion as CONFIRMED_3D and remembers it through a LOST period', () => {
    const tracker = new LiveTagTracker({ staleMs: 500 });
    tracker.ingest(input(cameraAt(5), 100));
    expect(tracker.states()[0].status).toBe('POSE_ESTIMATED');
    tracker.setConfirmed([2]);
    expect(tracker.states()[0].status).toBe('CONFIRMED_3D');
    expect(liveTagOverlayQuads(tracker.states(), projectWith(cameraAt(5)))[0].style).toBe('confirmed');
    tracker.tick(2000);
    expect(tracker.states()[0].status).toBe('LOST');
    tracker.ingest(input(cameraAt(5), 2100));
    expect(tracker.states()[0].status).toBe('CONFIRMED_3D');
    tracker.setConfirmed([]);
    tracker.ingest(input(cameraAt(5), 2200));
    expect(tracker.states()[0].status).toBe('POSE_ESTIMATED');
  });

  it('never confirms a tag that has no anchored pose, and omits quads with a corner behind the camera', () => {
    const tracker = new LiveTagTracker();
    const base = input(cameraAt(0), 100);
    tracker.ingest({ ...base, pose: { ok: false, reason: 'x' } });
    tracker.setConfirmed([2]);
    expect(tracker.states()[0].status).toBe('DETECTED_2D');
    const anchored = new LiveTagTracker();
    anchored.ingest(input(cameraAt(0), 100));
    const behind: CameraPose = { eye: worldToProject([0.1, 0, -2.5]), target: worldToProject([0.1, 0, -3.5]) };
    expect(liveTagOverlayQuads(anchored.states(), projectWith(behind))).toEqual([]);
  });

  it('tracks several tags independently and reset() forgets all of them (no carry-over between sessions)', () => {
    const tracker = new LiveTagTracker();
    tracker.ingest(input(cameraAt(0), 100, 0, 1, 1));
    tracker.ingest(input(cameraAt(0), 100, 0, 1, 3));
    expect(tracker.states().map((s) => s.node)).toEqual(['G1', 'G3']);
    tracker.reset();
    expect(tracker.states()).toEqual([]);
    expect(tracker.ingest(input(cameraAt(0), 200, 0, 1, 1))).toBe(true);
  });

  it('does not anchor a pose that carries no usable rotation', () => {
    const tracker = new LiveTagTracker();
    const base = input(cameraAt(0), 100);
    const noRotation = base.pose.ok ? ({ ...base.pose, rotationScan: undefined } as unknown as typeof base.pose) : base.pose;
    expect(() => tracker.ingest({ ...base, pose: noRotation })).not.toThrow();
    expect(tracker.states()[0].cornersWorld).toBeNull();
    const nan = base.pose.ok ? ({ ...base.pose, rotationScan: [NaN, 0, 0, 0, 1, 0, 0, 0, 1] } as unknown as typeof base.pose) : base.pose;
    const t2 = new LiveTagTracker();
    t2.ingest({ ...base, pose: nan });
    expect(t2.states()[0].cornersWorld).toBeNull();
  });

  it('converts the XR world to the project frame like the runtime does', () => {
    expect(xrToProject([1, 2, 3])).toEqual([1, -3, 2]);
    const corners = tagWorldCorners([1, 2, 3], [1, 0, 0, 0, 1, 0, 0, 0, 1], 0.1);
    expect(corners[0]).toEqual([1 - 0.05, 2 - 0.05, 3]);
    expect(corners[2]).toEqual([1 + 0.05, 2 + 0.05, 3]);
  });
});
