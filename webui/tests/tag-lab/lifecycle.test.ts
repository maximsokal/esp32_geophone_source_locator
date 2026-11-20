import { describe, expect, it, vi } from 'vitest';
import { classifyCameraError, listVideoDevices, WebcamFrameSource, type MediaDevicesLike } from '../../src/tag-lab/WebcamFrameSource';
import { scheduleVideoFrames } from '../../src/tag-lab/frameSource';
import { TagLabDiagnostics } from '../../src/tag-lab/TagLabDiagnostics';

class FakeTrack { stopped = false; label = 'Fake cam'; stop() { this.stopped = true; } getSettings() { return { width: 1280, height: 720 }; } }
class FakeStream { tracks = [new FakeTrack()]; getTracks() { return this.tracks; } getVideoTracks() { return this.tracks; } }
class FakeVideo {
  srcObject: unknown = null; muted = false; playsInline = false; videoWidth = 1280; videoHeight = 720; paused = false;
  playError: Error | null = null;
  callbacks = new Map<number, (now: number) => void>(); nextHandle = 1; cancelled: number[] = [];
  async play() { if (this.playError !== null) throw this.playError; }
  pause() { this.paused = true; }
  requestVideoFrameCallback(cb: (now: number) => void) { const h = this.nextHandle++; this.callbacks.set(h, cb); return h; }
  cancelVideoFrameCallback(h: number) { this.cancelled.push(h); this.callbacks.delete(h); }
  fire(now = 1) { const [h, cb] = [...this.callbacks][0]; this.callbacks.delete(h); cb(now); }
}

function media(streams: FakeStream[], error?: unknown): MediaDevicesLike & { requested: MediaStreamConstraints[] } {
  const requested: MediaStreamConstraints[] = [];
  return {
    requested,
    async getUserMedia(constraints) { requested.push(constraints); if (error !== undefined) throw error; const s = new FakeStream(); streams.push(s); return s as unknown as MediaStream; },
    async enumerateDevices() { return [{ kind: 'videoinput', deviceId: 'a', label: 'Cam A' }, { kind: 'audioinput', deviceId: 'm', label: 'Mic' }, { kind: 'videoinput', deviceId: 'b', label: '' }] as MediaDeviceInfo[]; },
  };
}
const named = (name: string, message = 'x') => Object.assign(new Error(message), { name });

describe('webcam frame source lifecycle', () => {
  it('starts with the requested device and size and reports the track settings', async () => {
    const streams: FakeStream[] = []; const m = media(streams); const video = new FakeVideo();
    const source = new WebcamFrameSource(video as never, m);
    const info = await source.start({ deviceId: 'b', width: 1280, height: 720 });
    expect(info).toEqual({ label: 'Fake cam', width: 1280, height: 720 });
    expect((m.requested[0].video as MediaTrackConstraints).deviceId).toEqual({ exact: 'b' });
    expect(video.srcObject).toBe(streams[0]);
    expect(source.active).toBe(true);
  });

  it('stops every track, detaches the stream and cancels frame callbacks on stop()', async () => {
    const streams: FakeStream[] = []; const video = new FakeVideo();
    const source = new WebcamFrameSource(video as never, media(streams));
    await source.start({ width: 640, height: 480 });
    const seen = vi.fn(); source.onFrame(seen);
    video.fire(); expect(seen).toHaveBeenCalledTimes(1);
    source.stop();
    expect(streams[0].tracks.every((t) => t.stopped)).toBe(true);
    expect(video.srcObject).toBeNull();
    expect(video.cancelled.length).toBeGreaterThan(0);
    expect(source.active).toBe(false);
  });

  it('releases the previous stream when switching devices', async () => {
    const streams: FakeStream[] = []; const source = new WebcamFrameSource(new FakeVideo() as never, media(streams));
    await source.start({ deviceId: 'a', width: 640, height: 480 });
    await source.start({ deviceId: 'b', width: 640, height: 480 });
    expect(streams).toHaveLength(2);
    expect(streams[0].tracks[0].stopped).toBe(true);
    expect(streams[1].tracks[0].stopped).toBe(false);
  });

  it('stops the new stream if playback fails, so no camera light stays on', async () => {
    const streams: FakeStream[] = []; const video = new FakeVideo(); video.playError = named('AbortError', 'play blocked');
    const source = new WebcamFrameSource(video as never, media(streams));
    await expect(source.start({ width: 640, height: 480 })).rejects.toThrow('play blocked');
    expect(streams[0].tracks[0].stopped).toBe(true);
    expect(source.active).toBe(false);
  });

  it('classifies permission denied, missing camera, busy camera and unsupported resolution', async () => {
    expect(classifyCameraError(named('NotAllowedError')).kind).toBe('permission-denied');
    expect(classifyCameraError(named('NotFoundError')).kind).toBe('no-camera');
    expect(classifyCameraError(named('NotReadableError')).kind).toBe('camera-busy');
    expect(classifyCameraError(named('OverconstrainedError')).kind).toBe('constraints');
    expect(classifyCameraError(named('TypeError')).kind).toBe('insecure-context');
    const source = new WebcamFrameSource(new FakeVideo() as never, media([], named('NotAllowedError', 'denied')));
    await expect(source.start({ width: 640, height: 480 })).rejects.toThrow('denied');
    expect(source.active).toBe(false);
  });

  it('fails clearly when mediaDevices is unavailable (insecure origin)', async () => {
    const source = new WebcamFrameSource(new FakeVideo() as never, undefined as never);
    await expect(source.start({ width: 640, height: 480 })).rejects.toMatchObject({ name: 'TypeError' });
  });

  it('lists only video inputs and names unlabeled ones', async () => {
    expect(await listVideoDevices(media([]))).toEqual([{ deviceId: 'a', label: 'Cam A' }, { deviceId: 'b', label: 'Camera 2' }]);
  });
});

describe('frame scheduling', () => {
  it('uses requestVideoFrameCallback when present and re-arms after every frame', () => {
    const video = new FakeVideo(); const cb = vi.fn();
    const cancel = scheduleVideoFrames(video as never, cb);
    video.fire(5); video.fire(6);
    expect(cb.mock.calls.map((c) => c[0])).toEqual([5, 6]);
    cancel(); expect(video.callbacks.size).toBe(0);
  });

  it('falls back to requestAnimationFrame without requestVideoFrameCallback', () => {
    const frames: ((n: number) => void)[] = []; const cancelled: number[] = [];
    vi.stubGlobal('requestAnimationFrame', (f: (n: number) => void) => { frames.push(f); return frames.length; });
    vi.stubGlobal('cancelAnimationFrame', (h: number) => { cancelled.push(h); });
    try {
      const cb = vi.fn(); const cancel = scheduleVideoFrames({} as never, cb);
      frames.shift()!(10); expect(cb).toHaveBeenCalledWith(10);
      cancel(); frames.shift()!(20); expect(cb).toHaveBeenCalledTimes(1);
      expect(cancelled.length).toBe(1);
    } finally { vi.unstubAllGlobals(); }
  });
});

describe('diagnostics transport', () => {
  const make = (fail = false) => {
    const bodies: { url: string; events?: unknown[] }[] = []; let t = 0;
    const d = new TagLabDiagnostics({ now: () => (t += 10), fetchImpl: async (url, init) => { if (fail) throw new Error('offline'); bodies.push({ url, events: JSON.parse(init.body).events }); return {}; } });
    return { d, bodies };
  };
  it('emits nothing outside a logging session and flushes LAB_START/SESSION_STOP inside one', async () => {
    const { d, bodies } = make();
    d.emit('LAB_ERROR', 'lab'); await d.flush(); expect(bodies).toHaveLength(0);
    d.startSession('webcam', { a: 1 }); d.emit('CAMERA_READY', 'webcam', { width: 1 });
    await d.stopSession('webcam', { reason: 'test' });
    const names = bodies.flatMap((b) => (b.events as { event: string }[]).map((e) => e.event));
    expect(names).toEqual(['LAB_START', 'CAMERA_READY', 'SESSION_STOP']);
    expect(bodies[0].url).toBe('/lab-log/events');
    expect(d.logging).toBe(false);
  });
  it('survives a missing log receiver and bounds its queue', async () => {
    const { d } = make(true);
    d.startSession('lab');
    for (let i = 0; i < 2500; i += 1) d.emit('CAMERA_FRAME', 'lab');
    expect(d.dropped).toBeGreaterThan(0);
    await d.flush(); expect(d.failed).toBeGreaterThan(0);
    d.dispose();
  });
});
