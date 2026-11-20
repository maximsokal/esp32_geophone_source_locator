import { beforeEach, describe, expect, it, vi } from 'vitest';
import { tagCaptureSizeFor, TAG_CAPTURE_PIXEL_BUDGET } from '../../src/xr/tagCaptureSize';

const captures: { width: number; height: number; disposed: number; captured: number; mode: string }[] = [];
const behaviour = { mode: 'nonblocking', throwOnCreate: false };
vi.mock('../../src/xr/rawCameraGuidance', async (importOriginal) => {
  const original = await importOriginal<typeof import('../../src/xr/rawCameraGuidance')>();
  class FakeCapture {
    record: (typeof captures)[number];
    constructor(_gl: unknown, width: number, height: number) {
      if (behaviour.throwOnCreate) throw new Error('GL out of memory');
      this.record = { width, height, disposed: 0, captured: 0, mode: behaviour.mode };
      captures.push(this.record);
    }
    get tagReadbackMode() { return this.record.mode; }
    pollReadyTag() { return null; }
    captureTag() { this.record.captured += 1; return null; }
    dispose() { this.record.disposed += 1; }
  }
  return { ...original, RawCameraGuidanceCapture: FakeCapture };
});

import { clientDiagnostics } from '../../src/data/clientDiagnostics';
import { WebXrRuntime } from '../../src/xr/webxrRuntime';

const view = (width: number, height: number) => ({
  camera: { width, height }, projectionMatrix: new Float32Array([1.9, 0, 0, 0, 0, 0.7, 0, 0, 0, 0, -1, -1, 0, 0, -0.2, 0]),
  transform: { matrix: new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]) },
});

describe('tag capture size follows the camera image', () => {
  it('keeps the pixel budget and the camera aspect, with even sides', () => {
    expect(tagCaptureSizeFor(1920, 1080)).toEqual({ width: 1280, height: 720 }); // unchanged for a 16:9 camera
    const portrait = tagCaptureSizeFor(886, 1920); // the phone in run 2026-11-20_165835
    expect(portrait).toEqual({ width: 652, height: 1414 });
    expect(portrait.width % 2).toBe(0);
    expect(portrait.height % 2).toBe(0);
    expect(Math.abs(portrait.width / portrait.height - 886 / 1920)).toBeLessThan(0.002);
    for (const [w, h] of [[886, 1920], [1920, 886], [640, 480], [1080, 1920], [4000, 3000]]) {
      const size = tagCaptureSizeFor(w, h);
      expect(Math.abs(size.width * size.height - TAG_CAPTURE_PIXEL_BUDGET) / TAG_CAPTURE_PIXEL_BUDGET).toBeLessThan(0.02);
      expect(Math.abs(size.width / size.height - w / h) / (w / h)).toBeLessThan(0.01);
    }
  });

  it('makes a tag square in the capture: no stretch for the portrait camera (the old 1280x720 target gave ~3.9x)', () => {
    const camera = { width: 886, height: 1920 };
    const stretch = (size: { width: number; height: number }) => (size.width / camera.width) / (size.height / camera.height);
    expect(stretch({ width: 1280, height: 720 })).toBeGreaterThan(3.8);
    expect(Math.abs(stretch(tagCaptureSizeFor(camera.width, camera.height)) - 1)).toBeLessThan(0.01);
  });

  it('falls back to 1280x720 for an unusable camera size and clamps extreme aspects', () => {
    for (const bad of [[0, 0], [NaN, 100], [-1, 5], [100, Infinity]]) expect(tagCaptureSizeFor(bad[0], bad[1])).toEqual({ width: 1280, height: 720 });
    const wide = tagCaptureSizeFor(10000, 100);
    expect(wide.width).toBeLessThanOrEqual(2048);
    expect(wide.height).toBeGreaterThanOrEqual(160);
  });
});

describe('WebXrRuntime tag capture target', () => {
  beforeEach(() => { captures.length = 0; behaviour.mode = 'nonblocking'; behaviour.throwOnCreate = false; });
  function runtimeWith(width = 1280, height = 720) {
    const runtime = new WebXrRuntime();
    const internal = runtime as unknown as Record<string, unknown> & { captureTagGuidance: (time: number, v: unknown) => void };
    const initial = { width, height, disposed: 0, captured: 0, mode: 'nonblocking' };
    captures.push(initial);
    Object.assign(internal, {
      state: { phase: 'running' }, gl: {}, cameraBinding: { getCameraImage: () => ({}) }, evidenceFrozen: false, tagCaptureEnabled: true, tagCaptureWidth: width, tagCaptureHeight: height,
      lastTagCaptureMs: Number.NEGATIVE_INFINITY,
      tagCameraCapture: { pollReadyTag: () => null, captureTag: () => { initial.captured += 1; return null; }, dispose: () => { initial.disposed += 1; }, tagReadbackMode: 'nonblocking' },
    });
    return { runtime, internal, initial };
  }

  it('re-creates the capture at the camera-matched size on the first frame, then captures at that size', () => {
    const info = vi.spyOn(clientDiagnostics, 'info');
    const { internal, initial } = runtimeWith();
    internal.captureTagGuidance(1000, view(886, 1920));
    expect(captures).toHaveLength(2);
    expect(captures[1]).toMatchObject({ width: 652, height: 1414 });
    expect(initial.disposed).toBe(1);
    expect(initial.captured).toBe(0); // the resize frame is skipped; the next interval captures
    expect(info).toHaveBeenCalledWith('XR_TAG_CAPTURE_RESIZED', expect.objectContaining({ fromWidth: 1280, fromHeight: 720, width: 652, height: 1414, cameraWidth: 886, cameraHeight: 1920 }));
    internal.captureTagGuidance(1400, view(886, 1920));
    expect(captures).toHaveLength(2);
    expect(captures[1].captured).toBe(1);
    expect(info).toHaveBeenCalledWith('XR_TAG_CAMERA_GEOMETRY', expect.objectContaining({ captureWidth: 652, captureHeight: 1414 }));
  });

  it('keeps the default capture for a 16:9 camera and re-creates it when the phone rotates', () => {
    const { internal, initial } = runtimeWith();
    internal.captureTagGuidance(1000, view(1920, 1080));
    expect(captures).toHaveLength(1);
    expect(initial.captured).toBe(1);
    internal.captureTagGuidance(1400, view(1080, 1920));
    expect(captures).toHaveLength(2);
    expect(captures[1]).toMatchObject({ width: 720, height: 1280 });
    expect(initial.disposed).toBe(1);
    internal.captureTagGuidance(1800, view(1920, 1080));
    expect(captures).toHaveLength(3);
    expect(captures[2]).toMatchObject({ width: 1280, height: 720 });
    expect(captures[1].disposed).toBe(1);
  });

  it('disables tag capture, and leaks nothing, if the matched size has no non-blocking readback or cannot be allocated', () => {
    behaviour.mode = 'synchronous';
    const a = runtimeWith();
    a.internal.captureTagGuidance(1000, view(886, 1920));
    expect(a.internal.tagCaptureEnabled).toBe(false);
    expect(captures[1].disposed).toBe(1);
    expect(a.initial.disposed).toBe(1);
    behaviour.mode = 'nonblocking'; behaviour.throwOnCreate = true;
    const b = runtimeWith();
    b.internal.captureTagGuidance(1000, view(886, 1920));
    expect(b.internal.tagCaptureEnabled).toBe(false);
    expect(b.initial.disposed).toBe(1);
  });
});
