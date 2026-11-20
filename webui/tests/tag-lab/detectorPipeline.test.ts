import { describe, expect, it } from 'vitest';
import { detectAprilTags } from '../../src/xr/fiducial/tagDetector';
import type { DetectedAprilTagFrame, TagCameraFrame } from '../../src/xr/fiducial/tagDetectorClient';
import { LabPipeline, type LabDetector, type LabFrame } from '../../src/tag-lab/labPipeline';
import { TagLabDiagnostics, type LabEvent } from '../../src/tag-lab/TagLabDiagnostics';
import { blankFrame, rasterTag } from './support/raster';

/** Runs the REAL project detector (same detectAprilTags the Worker calls) synchronously behind the client interface. */
class RealDetector implements LabDetector {
  busy = false;
  calls = 0;
  async detect(frame: TagCameraFrame): Promise<DetectedAprilTagFrame | null> {
    this.calls += 1;
    return {
      width: frame.width, height: frame.height, capturedAtMs: frame.capturedAtMs, cameraPosition: frame.cameraPosition, cameraForward: frame.cameraForward,
      projectionMatrix: Float32Array.from(frame.projectionMatrix), worldFromView: Float32Array.from(frame.worldFromView),
      detections: detectAprilTags(frame.width, frame.height, frame.rgba),
    };
  }
}

function harness(detector: LabDetector = new RealDetector()) {
  let t = 1000;
  const sent: LabEvent[] = [];
  const diagnostics = new TagLabDiagnostics({
    now: () => t,
    fetchImpl: async (_url, init) => { sent.push(...JSON.parse(init.body).events); return {}; },
  });
  diagnostics.startSession('webcam');
  const pipeline = new LabPipeline({ detector, diagnostics, now: () => t, staleMs: 1000 });
  const frame = (rgba: Uint8Array, id: number, epoch = pipeline.sessionEpoch): LabFrame => ({ frameId: id, sourceType: 'webcam', capturedAtMs: t, width: 400, height: 300, rgba: new Uint8Array(rgba), sessionEpoch: epoch });
  return { pipeline, diagnostics, sent, frame, advance: (ms: number) => { t += ms; }, flush: async () => { await diagnostics.flush(); return sent; } };
}

describe('tag lab detection pipeline (real detector)', () => {
  it('decodes each sheet tag 0-4 and reports the bound device name immediately as DETECTED 2D', async () => {
    const expected = ['MAIN', 'G1', 'G2', 'G3', 'G4'];
    for (let id = 0; id <= 4; id += 1) {
      const h = harness();
      expect(await h.pipeline.submit(h.frame(rasterTag(id).rgba, 1))).toBe('processed');
      const state = h.pipeline.tagStates().find((s) => s.tagId === id)!;
      expect(state.status).toBe('DETECTED 2D');
      expect(state.node).toBe(expected[id]);
      expect(state.corners).toHaveLength(4);
      expect(h.pipeline.tagStates().filter((s) => s.status === 'DETECTED 2D')).toHaveLength(1);
      expect(h.pipeline.lastDetectedTagId).toBe(id);
    }
  });

  it('decodes a small 12 px/module tag and leaves a blank frame at NOT SEEN', async () => {
    const h = harness();
    await h.pipeline.submit(h.frame(rasterTag(3, { cell: 12 }).rgba, 1));
    expect(h.pipeline.tagStates().find((s) => s.tagId === 3)!.status).toBe('DETECTED 2D');
    const blank = harness();
    await blank.pipeline.submit(blank.frame(blankFrame(), 1));
    expect(blank.pipeline.tagStates().every((s) => s.status === 'NOT SEEN')).toBe(true);
    expect(blank.pipeline.counters.decoded).toBe(0);
  });

  it('marks a tag LOST after the stale timeout and logs it, then recovers on the next decode', async () => {
    const h = harness();
    await h.pipeline.submit(h.frame(rasterTag(1).rgba, 1));
    h.advance(900); h.pipeline.tick();
    expect(h.pipeline.tagStates()[1].status).toBe('DETECTED 2D');
    h.advance(200); h.pipeline.tick();
    expect(h.pipeline.tagStates()[1].status).toBe('LOST');
    expect((await h.flush()).some((e) => e.event === 'TAG_LOST' && e.fields.tagId === 1)).toBe(true);
    await h.pipeline.submit(h.frame(rasterTag(1).rgba, 2));
    expect(h.pipeline.tagStates()[1].status).toBe('DETECTED 2D');
  });

  it('drops frames while a decode is pending instead of queueing them', async () => {
    let release: (() => void) | undefined;
    const slow: LabDetector = {
      get busy() { return release !== undefined; },
      detect: (frame) => new Promise((resolve) => { release = () => { release = undefined; resolve({ width: frame.width, height: frame.height, capturedAtMs: frame.capturedAtMs, cameraPosition: frame.cameraPosition, cameraForward: frame.cameraForward, projectionMatrix: new Float32Array(16), worldFromView: new Float32Array(16), detections: [] }); }; }),
    };
    const h = harness(slow);
    const first = h.pipeline.submit(h.frame(blankFrame(), 1));
    expect(await h.pipeline.submit(h.frame(blankFrame(), 2))).toBe('dropped-busy');
    expect(await h.pipeline.submit(h.frame(blankFrame(), 3))).toBe('dropped-busy');
    expect(h.pipeline.counters.framesDropped).toBe(2);
    release!();
    expect(await first).toBe('processed');
    expect(h.pipeline.counters.framesProcessed).toBe(1);
    expect((await h.flush()).filter((e) => e.event === 'WORKER_BUSY')).toHaveLength(1);
  });

  it('does not let a result from an old session epoch reappear after newSession()', async () => {
    let finish: (() => void) | undefined;
    const real = new RealDetector();
    const delayed: LabDetector = { busy: false, detect: async (frame) => { const result = await real.detect(frame); await new Promise<void>((r) => { finish = r; }); return result; } };
    const h = harness(delayed);
    const pending = h.pipeline.submit(h.frame(rasterTag(2).rgba, 1));
    await Promise.resolve(); await Promise.resolve();
    h.pipeline.newSession();
    finish!();
    expect(await pending).toBe('dropped-stale-epoch');
    expect(h.pipeline.tagStates().every((s) => s.status === 'NOT SEEN')).toBe(true);
    expect(h.pipeline.counters.decoded).toBe(0);
  });

  it('clears every tag and counter when a new session starts, so old detections never reappear', async () => {
    const h = harness();
    await h.pipeline.submit(h.frame(rasterTag(2).rgba, 1));
    expect(h.pipeline.tagStates().find((s) => s.tagId === 2)!.status).toBe('DETECTED 2D');
    h.pipeline.newSession();
    expect(h.pipeline.tagStates().every((s) => s.status === 'NOT SEEN' && s.corners === null && s.detectionCount === 0)).toBe(true);
    expect(h.pipeline.counters.decoded).toBe(0);
    expect(h.pipeline.lastDetectedTagId).toBeNull();
  });

  it('does not report a disposed detector rejecting its pending decode after close() as a fault, and stops LOST decay', async () => {
    let reject: ((e: Error) => void) | undefined;
    const hanging: LabDetector = { busy: false, detect: () => new Promise((_, r) => { reject = r; }) };
    const h = harness(hanging);
    const pending = h.pipeline.submit(h.frame(blankFrame(), 1));
    h.pipeline.close();
    reject!(new Error('Tag detector client disposed.'));
    expect(await pending).toBe('dropped-stale-epoch');
    expect(h.pipeline.lastError).toBeNull();
    expect(h.pipeline.counters.workerErrors).toBe(0);
    const seen = harness();
    await seen.pipeline.submit(seen.frame(rasterTag(1).rgba, 1));
    seen.pipeline.close(); seen.advance(5000); seen.pipeline.tick();
    expect(seen.pipeline.tagStates()[1].status).toBe('DETECTED 2D');
  });

  it('rejects frames stamped with another epoch and reports worker faults without crashing', async () => {
    const h = harness();
    expect(await h.pipeline.submit(h.frame(blankFrame(), 1, 99))).toBe('dropped-stale-epoch');
    const failing: LabDetector = { busy: false, detect: async () => { throw new Error('worker exploded'); } };
    const f = harness(failing);
    expect(await f.pipeline.submit(f.frame(blankFrame(), 1))).toBe('error');
    expect(f.pipeline.lastError).toBe('worker exploded');
    expect((await f.flush()).some((e) => e.event === 'WORKER_ERROR' && e.fields.message === 'worker exploded')).toBe(true);
  });

  it('aggregates frame metrics once per second instead of one event per frame', async () => {
    const h = harness();
    for (let i = 0; i < 30; i += 1) { await h.pipeline.submit(h.frame(blankFrame(), i)); h.advance(30); h.pipeline.aggregate(); }
    const events = await h.flush();
    expect(events.filter((e) => e.event === 'CAMERA_FRAME').length).toBeLessThanOrEqual(2);
    expect(events.filter((e) => e.event === 'DECODE_STARTED')).toHaveLength(1);
  });

  it('logs TAG_DETECTED once per appearance with hamming distance and corners', async () => {
    const h = harness();
    for (let i = 0; i < 4; i += 1) { await h.pipeline.submit(h.frame(rasterTag(4).rgba, i)); h.advance(100); }
    const detected = (await h.flush()).filter((e) => e.event === 'TAG_DETECTED');
    expect(detected).toHaveLength(1);
    expect(detected[0].fields).toMatchObject({ tagId: 4, node: 'G4', hamming: 0 });
    expect((detected[0].fields.corners as unknown[]).length).toBe(4);
  });
});
