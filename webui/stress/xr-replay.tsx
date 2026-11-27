import { createRoot } from 'react-dom/client';

import LiveSeismicPanel from '../src/features/seismic/LiveSeismicPanel';
import { LocaleProvider } from '../src/i18n';
import { drawSyntheticScene } from '../src/tag-lab/ReplayFrameSource';
import { DepthPointCloudAccumulator } from '../src/xr/depthPointCloud';
import { TagDetectorClient } from '../src/xr/fiducial/tagDetectorClient';
import { LiveMeshRenderer } from '../src/xr/liveMeshRenderer';
import { WebXrRuntime } from '../src/xr/webxrRuntime';
import { FakeXrLayer, FakeXrSession, FakeXrSystem } from '../tests/xr/support/xrFakes';

/*
 * A whole scanning session replayed through the PRODUCTION runtime in Chromium: the real WebXrRuntime, real WebGL, the real fusion, live-mesh and AprilTag
 * workers and the real live seismic panel. Only the immersive-session protocol (frames, poses, depth) is simulated, from recorded depth/pose packs, and the
 * seismic endpoint answers synthetic data. It reads the runtime only through what exists in the baseline too, so the same page measures both versions.
 * DESKTOP Chromium: it says nothing about a phone's absolute numbers, thermals or ARCore.
 */
interface Pack { name: string; width: number; height: number; projection: Float32Array; frames: Array<{ depth: ArrayBuffer; worldFromView: Float32Array; eye: [number, number, number]; keyframeId: number; keyframe: boolean }> }
export type Scenario = 'scan' | 'static' | 'pause' | 'tracking-loss' | 'context-loss';
export interface ReplayOptions { scenario: Scenario; durationMs: number; seismic: boolean; tags: boolean }

const PACK_FRAME_MS = 100;                       /* the packs hold every third frame of 30 Hz sequences */
const TAG_PERIOD_MS = 330;

function parsePack(buffer: ArrayBuffer): Pack {
  const view = new DataView(buffer);
  const headerBytes = view.getUint32(0, true);
  const header = JSON.parse(new TextDecoder().decode(new Uint8Array(buffer, 4, headerBytes))) as { name: string; frames: number; width: number; height: number; projection: number[] };
  const depthBytes = header.width * header.height * 2;
  const frameBytes = depthBytes + 16 * 4 + 3 * 4 + 4 + 4;
  const frames: Pack['frames'] = [];
  for (let i = 0; i < header.frames; i += 1) {
    const base = 4 + headerBytes + i * frameBytes;
    const matrix = new Float32Array(16); for (let k = 0; k < 16; k += 1) matrix[k] = view.getFloat32(base + depthBytes + k * 4, true);
    const eyeBase = base + depthBytes + 64;
    frames.push({
      depth: buffer.slice(base, base + depthBytes), worldFromView: matrix,
      eye: [view.getFloat32(eyeBase, true), view.getFloat32(eyeBase + 4, true), view.getFloat32(eyeBase + 8, true)],
      keyframeId: view.getUint32(eyeBase + 12, true), keyframe: view.getUint32(eyeBase + 16, true) === 1,
    });
  }
  return { name: header.name, width: header.width, height: header.height, projection: Float32Array.from(header.projection), frames };
}

const IDENTITY = Float32Array.from([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);

function deviceDepth(pack: Pack, index: number) {
  const { width, height } = pack; const frame = pack.frames[index];
  const values = new Uint16Array(frame.depth);
  return {
    width, height, data: frame.depth, rawValueToMeters: 0.001, normDepthBufferFromNormView: { matrix: IDENTITY },
    projectionMatrix: pack.projection, transform: { matrix: frame.worldFromView },
    getDepthInMeters(x: number, y: number): number { const v = values[Math.min(height - 1, Math.trunc(y * height)) * width + Math.min(width - 1, Math.trunc(x * width))]; return v === 0 ? Number.NaN : v * 0.001; },
  };
}

function quaternion(m: Float32Array): { x: number; y: number; z: number; w: number } {
  const trace = m[0] + m[5] + m[10];
  if (trace > 0) { const s = Math.sqrt(trace + 1) * 2; return { w: s / 4, x: (m[6] - m[9]) / s, y: (m[8] - m[2]) / s, z: (m[1] - m[4]) / s }; }
  if (m[0] > m[5] && m[0] > m[10]) { const s = Math.sqrt(1 + m[0] - m[5] - m[10]) * 2; return { w: (m[6] - m[9]) / s, x: s / 4, y: (m[4] + m[1]) / s, z: (m[8] + m[2]) / s }; }
  if (m[5] > m[10]) { const s = Math.sqrt(1 + m[5] - m[0] - m[10]) * 2; return { w: (m[8] - m[2]) / s, x: (m[4] + m[1]) / s, y: s / 4, z: (m[9] + m[6]) / s }; }
  const s = Math.sqrt(1 + m[10] - m[0] - m[5]) * 2; return { w: (m[1] - m[4]) / s, x: (m[8] + m[2]) / s, y: (m[9] + m[6]) / s, z: s / 4 };
}

function viewerPose(pack: Pack, index: number) {
  const matrix = pack.frames[index].worldFromView;
  const transform = { matrix, position: { x: matrix[12], y: matrix[13], z: matrix[14] }, orientation: quaternion(matrix) };
  return { transform, views: [{ projectionMatrix: pack.projection, transform, camera: null }] };
}

/** The sequence forward then backward, so a long replay has no jump in the pose. */
const pingPong = (step: number, length: number): number => { const period = Math.max(1, 2 * (length - 1)); const k = step % period; return k < length ? k : period - k; };

const quantile = (sorted: number[], q: number): number => (sorted.length === 0 ? 0 : sorted[Math.min(sorted.length - 1, Math.max(0, Math.ceil(q * sorted.length) - 1))]);
const summarize = (values: number[]) => { const sorted = values.slice().sort((a, b) => a - b); return { n: values.length, mean: values.length ? values.reduce((a, b) => a + b, 0) / values.length : 0, p50: quantile(sorted, 0.5), p95: quantile(sorted, 0.95), p99: quantile(sorted, 0.99), max: sorted.at(-1) ?? 0 }; };
const heapMb = (): number => ((performance as unknown as { memory?: { usedJSHeapSize: number } }).memory?.usedJSHeapSize ?? 0) / 1048576;
const sleep = (ms: number): Promise<void> => new Promise((resolve) => setTimeout(resolve, ms));

/** Recall and precision of `got` against `reference` surfels, 3 cm. */
function agreement(reference: Float32Array, got: Float32Array): { recall: number; precision: number } {
  const TOLERANCE = 0.03;
  const covered = (a: Float32Array, b: Float32Array): number => {
    if (a.length === 0) return 0;
    const grid = new Map<string, number[]>();
    for (let i = 0; i + 2 < b.length; i += 3) { const key = `${Math.floor(b[i] / TOLERANCE)},${Math.floor(b[i + 1] / TOLERANCE)},${Math.floor(b[i + 2] / TOLERANCE)}`; const list = grid.get(key); if (list) list.push(i); else grid.set(key, [i]); }
    let hits = 0;
    for (let i = 0; i + 2 < a.length; i += 3) {
      const cx = Math.floor(a[i] / TOLERANCE); const cy = Math.floor(a[i + 1] / TOLERANCE); const cz = Math.floor(a[i + 2] / TOLERANCE);
      search: for (let dx = -1; dx <= 1; dx += 1) for (let dy = -1; dy <= 1; dy += 1) for (let dz = -1; dz <= 1; dz += 1) for (const j of grid.get(`${cx + dx},${cy + dy},${cz + dz}`) ?? []) {
        if (Math.hypot(a[i] - b[j], a[i + 1] - b[j + 1], a[i + 2] - b[j + 2]) <= TOLERANCE) { hits += 1; break search; }
      }
    }
    return hits / (a.length / 3);
  };
  return { recall: covered(reference, got), precision: covered(got, reference) };
}

/* ------------------------------------------------------------------ the simulated platform */
const xr = new FakeXrSystem();
const sessions: FakeXrSession[] = [];
const requestSession = xr.requestSession.bind(xr);
xr.requestSession = () => {
  const session = new FakeXrSession();
  Object.assign(session, { depthUsage: 'cpu-optimized', depthDataFormat: 'unsigned-short', depthActive: true });
  sessions.push(session); xr.queue(session); return requestSession();
};
Object.defineProperty(navigator, 'xr', { configurable: true, value: xr });
Object.assign(window, { XRWebGLLayer: FakeXrLayer, XRWebGLBinding: undefined });
for (const prototype of [WebGLRenderingContext.prototype, WebGL2RenderingContext.prototype]) Object.defineProperty(prototype, 'makeXRCompatible', { configurable: true, value: async () => {} });

const seismicCalls: number[] = [];
let seismicSample = 0;
function seismicBody(): string {
  seismicSample += 220;
  const points = Array.from({ length: 220 }, (_, i) => {
    const t = (seismicSample + i) * 2000;
    return { timestamp_us: t, end_timestamp_us: t + 1999, channels: [1, 2, 3, 4].map((id) => { const v = Math.sin((seismicSample + i) / (7 + id)) * 400 * id; return { id, min_uv: v - 20, max_uv: v + 20 }; }) };
  });
  return JSON.stringify({ source: 'replay', sample_rate_hz: 500, count: 220, frame_count: 220, requested_span_ms: 440, span_ms: 440, decimation_factor: 1, buffered_frames: 4096, total_frames: seismicSample, points });
}
const nativeFetch = window.fetch.bind(window);
window.fetch = async (input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
  const url = String(input instanceof Request ? input.url : input);
  if (!url.includes('/api/')) return nativeFetch(input, init);
  if (url.includes('/api/seismic?')) { seismicCalls.push(performance.now()); return new Response(seismicBody(), { headers: { 'Content-Type': 'application/json' } }); }
  return new Response('{}', { headers: { 'Content-Type': 'application/json' } });
};

/* ------------------------------------------------------------------ one session */
async function replay(pack: Pack, options: ReplayOptions) {
  const runtime = new WebXrRuntime();
  runtime.attach();
  const internals = runtime as unknown as Record<string, any>;
  const captures: number[] = [];                  /* times at which a depth frame was handed to the fusion */
  const meshLatency: number[] = [];
  let meshInputAt: number | null = null; let requestedAt: number | null = null;
  const originalCaptureDepth = internals.captureDepth.bind(runtime);
  internals.captureDepth = (...args: unknown[]) => { const before = internals.sampledDepthFrames as number; originalCaptureDepth(...args); if ((internals.sampledDepthFrames as number) > before) captures.push(performance.now()); };
  /* The moment the live-mesh input is taken from the fusion state: asked of the worker now, built on the spot before. */
  const fusion = internals.pointCloud;
  if (typeof fusion.requestLiveMeshInput === 'function') {
    const request = fusion.requestLiveMeshInput.bind(fusion); const take = fusion.takeLiveMeshInput.bind(fusion);
    fusion.requestLiveMeshInput = (max: number) => { const idle = !fusion.liveMeshFetching && fusion.liveMeshInput === null; if (idle) requestedAt = performance.now(); request(max); };
    fusion.takeLiveMeshInput = () => { const value = take(); if (value !== null) meshInputAt = requestedAt; return value; };
  } else {
    const build = fusion.liveMeshInput.bind(fusion);
    fusion.liveMeshInput = (max: number) => { meshInputAt = performance.now(); return build(max); };
  }
  let meshUploads = 0; let meshTriangles = 0;
  const originalUpdate = LiveMeshRenderer.prototype.update;
  LiveMeshRenderer.prototype.update = function patched(this: LiveMeshRenderer, ...args: Parameters<typeof originalUpdate>) {
    originalUpdate.apply(this, args);
    meshUploads += 1; meshTriangles = args[1] as number;
    if (meshInputAt !== null) { const newest = [...captures].reverse().find((time) => time <= meshInputAt!); if (newest !== undefined) meshLatency.push(performance.now() - newest); meshInputAt = null; }
  };

  const seismicRoot = document.getElementById('seismic')!; const react = createRoot(seismicRoot);
  seismicCalls.length = 0;
  if (options.seismic) react.render(<LocaleProvider><LiveSeismicPanel /></LocaleProvider>);

  const longTasks: number[] = [];
  const observer = new PerformanceObserver((list) => { for (const entry of list.getEntries()) longTasks.push(entry.duration); });
  observer.observe({ entryTypes: ['longtask'] });

  await runtime.start();
  if (runtime.getState().phase !== 'running') throw new Error(`the runtime did not start: ${runtime.getState().error}`);
  runtime.setLiveMeshMode('auto');
  const session = sessions.at(-1)!;

  /* AprilTag: the production detector client and worker on pre-rendered camera frames of the five sheet tags. */
  const tagLatency: number[] = []; let tagBusy = 0; let tagDetections = 0; let tagSent = 0;
  let tagTimer: number | null = null; let tagClient: TagDetectorClient | null = null;
  if (options.tags) {
    const canvas = document.createElement('canvas'); canvas.width = 1280; canvas.height = 720;
    const ctx = canvas.getContext('2d', { willReadFrequently: true })!;
    const rendered = Array.from({ length: 12 }, (_, i) => { drawSyntheticScene(ctx, i * 700, 1280, 720); return new Uint8Array(ctx.getImageData(0, 0, 1280, 720).data.buffer); });
    tagClient = new TagDetectorClient();
    let n = 0;
    tagTimer = window.setInterval(() => {
      if (tagClient!.busy) { tagBusy += 1; return; }
      const started = performance.now(); tagSent += 1;
      void tagClient!.detect({ width: 1280, height: 720, rgba: rendered[n++ % rendered.length].slice(), rgbaOwnership: 'owned', capturedAtMs: started, cameraPosition: [0, 0, 0], cameraForward: [0, 0, -1], projectionMatrix: pack.projection, worldFromView: IDENTITY })
        .then((result) => { if (result !== null) { tagLatency.push(performance.now() - started); tagDetections += result.detections.length; } });
    }, TAG_PERIOD_MS);
  }

  const frameWork: number[] = []; const frameDelta: number[] = []; const queueDepths: number[] = []; const heap: number[] = [];
  let previous: number | null = null; let lastHeapAt = 0; let paused = false; let contextLost = false; const phases = new Set<string>();
  const started = performance.now();
  let packClockMs = 0; let lastTick = started;
  const at = (fraction: number): number => fraction * options.durationMs;
  const inWindow = (elapsed: number, from: number, length: number): boolean => elapsed >= at(from) && elapsed < at(from + length);
  await new Promise<void>((resolve, reject) => {
    const tick = (now: number): void => {
      try {
        const elapsed = performance.now() - started;
        const dt = performance.now() - lastTick; lastTick = performance.now();
        if (elapsed >= options.durationMs) { resolve(); return; }
        if (previous !== null) frameDelta.push(now - previous);
        previous = now;
        const hidden = options.scenario === 'pause' && inWindow(elapsed, 0.4, 0.2);
        if (hidden !== paused) { paused = hidden; session.visibilityState = hidden ? 'hidden' : 'visible'; session.emitVisibilityChange(); }
        if (!hidden) packClockMs += dt;
        const lost = options.scenario === 'tracking-loss' && (inWindow(elapsed, 0.3, 0.1) || inWindow(elapsed, 0.65, 0.1));
        if (options.scenario === 'context-loss' && !contextLost && elapsed >= at(0.5)) { contextLost = true; internals.gl?.getExtension('WEBGL_lose_context')?.loseContext(); }
        const index = options.scenario === 'static' ? Math.floor(pack.frames.length * 0.25) : pingPong(Math.floor(packClockMs / PACK_FRAME_MS), pack.frames.length);
        if (session.liveFrameCallbacks > 0) {
          const frame = { session, getViewerPose: () => (lost ? null : viewerPose(pack, index)), getDepthInformation: () => deviceDepth(pack, index) };
          const t0 = performance.now();
          session.driveNextFrame(now, frame as never);
          frameWork.push(performance.now() - t0);
        }
        phases.add(runtime.getState().phase);
        queueDepths.push(fusion.client?.queueDepth ?? 0);
        if (elapsed - lastHeapAt >= 1000) { lastHeapAt = elapsed; heap.push(heapMb()); }
        requestAnimationFrame(tick);
      } catch (error) { reject(error as Error); }
    };
    requestAnimationFrame(tick);
  });
  if (tagTimer !== null) window.clearInterval(tagTimer);
  const seismicIntervals = seismicCalls.slice(1).map((time, i) => time - seismicCalls[i]);
  const stateBeforeFinish = { ...runtime.getState() };
  const counters = fusion.client?.counters ?? null;
  const countersBeforeFinish = counters === null ? null : { accepted: counters.accepted as number, droppedOptional: counters.droppedOptional as number, droppedKeyframes: counters.droppedKeyframes as number, lostFrames: counters.lostFrames as number, maxQueueDepth: counters.maxQueueDepth as number, maxJournal: counters.maxJournal as number, workerMsTotal: counters.workerMsTotal as number, checkpointMsTotal: (counters.checkpointMsTotal ?? 0) as number, recoveries: counters.recoveries as number };
  const governor = fusion.governor === undefined ? null : { ...fusion.governor.counters, intervalMs: fusion.governor.intervalMs as number };

  let finishMs: number | null = null; let finishError: string | null = null; let snapshot: { positions: Float32Array; pointCount: number } | null = null;
  if (runtime.getState().phase === 'running') {
    const t0 = performance.now();
    try { await runtime.stop(); finishMs = performance.now() - t0; snapshot = runtime.getPointCloudSnapshot(); }
    catch (error) { finishError = error instanceof Error ? error.message : String(error); }
  }
  await sleep(200);
  observer.disconnect();
  const finalState = runtime.getState();
  react.unmount();
  tagClient?.dispose();
  LiveMeshRenderer.prototype.update = originalUpdate;
  await runtime.dispose();

  let quality: { recall: number; precision: number; referencePoints: number } | null = null;
  if (snapshot !== null && options.scenario !== 'static') {
    const reference = new DepthPointCloudAccumulator();
    pack.frames.forEach((frame, i) => reference.sampleDepthFrame(deviceDepth(pack, i), { projectionMatrix: pack.projection, transform: { matrix: frame.worldFromView } }, { keyframeId: frame.keyframeId, evidenceKeyframe: frame.keyframe, frameWeight: frame.keyframe ? 1 : 0.2, cameraProjectPosition: frame.eye }));
    const full = reference.snapshot(pack.frames.length, pack.frames.length);
    quality = { ...agreement(full.positions, snapshot.positions), referencePoints: full.pointCount };
  }

  return {
    scenario: options.scenario, durationMs: options.durationMs,
    frames: frameWork.length, frameWork: summarize(frameWork), frameDelta: summarize(frameDelta), longFrames50: frameDelta.filter((d) => d >= 50).length,
    longTasks: { count: longTasks.length, totalMs: longTasks.reduce((a, b) => a + b, 0), maxMs: Math.max(0, ...longTasks) },
    depth: { xrDepthFrames: stateBeforeFinish.depthFrameCount, integrated: stateBeforeFinish.sampledDepthFrames, keyframes: stateBeforeFinish.keyframeCount, points: stateBeforeFinish.pointCount, motionRejected: stateBeforeFinish.motionRejectedFrames },
    fusion: countersBeforeFinish, governor, queueDepth: summarize(queueDepths),
    mesh: { uploads: meshUploads, triangles: meshTriangles, depthToMeshMs: summarize(meshLatency) },
    tags: { sent: tagSent, busy: tagBusy, answered: tagLatency.length, detections: tagDetections, latencyMs: summarize(tagLatency) },
    seismic: { requests: seismicCalls.length, intervalMs: summarize(seismicIntervals), rendered: seismicSample / 220 },
    heapMb: { first: heap[0] ?? 0, peak: Math.max(0, ...heap), last: heap.at(-1) ?? 0, samples: heap.length },
    finishMs, finishError, finalPoints: snapshot?.pointCount ?? null, quality,
    contextLost, phases: [...phases], finalPhase: finalState.phase, finalError: finalState.error, liveSessions: xr.liveSessions, pendingFrames: session.liveFrameCallbacks,
  };
}

(window as unknown as { __xrReplay: unknown }).__xrReplay = {
  load: async (url: string) => parsePack(await (await nativeFetch(url)).arrayBuffer()),
  run: (pack: Pack, options: ReplayOptions) => replay(pack, options),
};
document.getElementById('state')!.textContent = 'ready';
