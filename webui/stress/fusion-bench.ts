import { DepthPointCloudAccumulator } from '../src/xr/depthPointCloud';
import type { DepthFusionFrameContext, ScanPointCloudSnapshot } from '../src/xr/depthPointCloudV2';
import { RuntimeFusion } from '../src/xr/depthFusionRuntime';
import { createDepthFusionWorker } from '../src/xr/depthFusionWorkerFactory';
import { captureFusionState } from '../src/xr/depthFusionCheckpoint';
import { buildSessionOccupancyArtifact } from '../src/xr/occupancySolverArtifact';
import { encodeScanArtifactWithIdentity } from '../src/xr/scanArtifact';
import { buildVoxelPreviewArtifact } from '../src/xr/voxelPreview';

/*
 * Browser benchmark of the fusion hand-off (Phase 2, stage 1): for the same depth/pose frames, the cost on the MAIN thread of (a) fusing synchronously, as the
 * runtime did before, and (b) handing the frame to the fusion worker (real Worker, real transfer). The worker's own CPU time and the latency from hand-off to
 * "fused" are reported separately; moving work off the main thread is not, by itself, evidence that the whole pipeline got faster.
 */
interface Pack { name: string; width: number; height: number; projection: Float32Array; frames: Array<{ depth: ArrayBuffer; worldFromView: Float32Array; eye: [number, number, number]; keyframeId: number; keyframe: boolean }> }

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

/** A device-like depth object: the raw uint16 buffer, rawValueToMeters and the normalising transform, exactly what XRCPUDepthInformation exposes. */
function deviceDepth(pack: Pack, index: number) {
  const { width, height } = pack; const frame = pack.frames[index];
  const values = new Uint16Array(frame.depth);
  const identity = Float32Array.from([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
  return {
    width, height, data: frame.depth, rawValueToMeters: 0.001, normDepthBufferFromNormView: { matrix: identity },
    projectionMatrix: pack.projection, transform: { matrix: frame.worldFromView },
    getDepthInMeters(x: number, y: number): number { const v = values[Math.min(height - 1, Math.trunc(y * height)) * width + Math.min(width - 1, Math.trunc(x * width))]; return v === 0 ? Number.NaN : v * 0.001; },
  };
}

const sleep = (ms: number): Promise<void> => new Promise((resolve) => setTimeout(resolve, ms));
const quantile = (sorted: number[], q: number): number => (sorted.length === 0 ? 0 : sorted[Math.min(sorted.length - 1, Math.max(0, Math.ceil(q * sorted.length) - 1))]);
const summarize = (values: number[]) => { const sorted = values.slice().sort((a, b) => a - b); const sum = values.reduce((a, b) => a + b, 0); return { n: values.length, mean: values.length ? sum / values.length : 0, p50: quantile(sorted, 0.5), p95: quantile(sorted, 0.95), p99: quantile(sorted, 0.99), max: sorted.at(-1) ?? 0, sum }; };
const heap = (): number => ((performance as unknown as { memory?: { usedJSHeapSize: number } }).memory?.usedJSHeapSize ?? 0) / 1048576;

function contextOf(frame: Pack['frames'][number]): DepthFusionFrameContext { return { keyframeId: frame.keyframeId, evidenceKeyframe: frame.keyframe, frameWeight: frame.keyframe ? 1 : 0.2, cameraProjectPosition: frame.eye }; }

async function runSync(pack: Pack, paceMs: number) {
  const accumulator = new DepthPointCloudAccumulator();
  const heapStart = heap(); const mainMs: number[] = [];
  for (let i = 0; i < pack.frames.length; i += 1) {
    if (paceMs > 0) await sleep(paceMs);
    const depth = deviceDepth(pack, i);
    const t0 = performance.now();
    accumulator.sampleDepthFrame(depth, { projectionMatrix: pack.projection, transform: { matrix: pack.frames[i].worldFromView } }, contextOf(pack.frames[i]));
    mainMs.push(performance.now() - t0);
  }
  const t0 = performance.now();
  const snapshot = accumulator.snapshot(pack.frames.length, pack.frames.length);
  const finishMs = performance.now() - t0;
  return { mainMs, finishMs, endToEndLatencyMs: [] as number[], workerMsTotal: 0, points: snapshot.pointCount, promoted: accumulator.promotedCount, dropped: 0, droppedKeyframes: 0, maxQueue: 0, heapDeltaMb: heap() - heapStart };
}

async function runWorker(pack: Pack, paceMs: number) {
  const fusion = new RuntimeFusion({ workerFactory: createDepthFusionWorker, maxInFlight: 3, mandatoryReserve: 5 });
  fusion.reset();
  if (fusion.mode !== 'worker') throw new Error('the fusion worker could not be started');
  const heapStart = heap(); const mainMs: number[] = [];
  let accepted = 0;
  for (let i = 0; i < pack.frames.length; i += 1) {
    if (paceMs > 0) await sleep(paceMs);
    const depth = deviceDepth(pack, i);
    const t0 = performance.now();
    const outcome = fusion.submitFrame(depth, { projectionMatrix: pack.projection, transform: { matrix: pack.frames[i].worldFromView } }, contextOf(pack.frames[i]), 'unsigned-short', t0);
    mainMs.push(performance.now() - t0);
    if (outcome === 'accepted') accepted += 1;
  }
  const t0 = performance.now();
  const result = await fusion.finish(pack.frames.length, accepted);
  const finishMs = performance.now() - t0;
  const counters = fusion.client.counters;
  const out = { mainMs, finishMs, endToEndLatencyMs: counters.frameRoundTripMs.slice(), workerMsTotal: counters.workerMsTotal, points: result.snapshot.pointCount, promoted: fusion.promotedCount, dropped: counters.droppedOptional, droppedKeyframes: counters.droppedKeyframes, maxQueue: counters.maxQueueDepth, heapDeltaMb: heap() - heapStart, drained: result.drained };
  fusion.dispose();
  return out;
}

/** Bit-exact comparison of two results: typed arrays byte for byte, everything else structurally. @returns the paths that differ */
function differences(a: unknown, b: unknown, path = 'value', out: string[] = []): string[] {
  if (out.length > 20) return out;
  if (ArrayBuffer.isView(a) || ArrayBuffer.isView(b)) {
    if (!ArrayBuffer.isView(a) || !ArrayBuffer.isView(b) || a.constructor !== b.constructor || a.byteLength !== b.byteLength) { out.push(`${path} (shape)`); return out; }
    const x = new Uint8Array(a.buffer, a.byteOffset, a.byteLength); const y = new Uint8Array(b.buffer, b.byteOffset, b.byteLength);
    for (let i = 0; i < x.length; i += 1) if (x[i] !== y[i]) { out.push(`${path} (byte ${i})`); break; }
    return out;
  }
  if (a instanceof Map || a instanceof Set || b instanceof Map || b instanceof Set) {
    if (!(a instanceof Map || a instanceof Set) || !(b instanceof Map || b instanceof Set)) { out.push(`${path} (container)`); return out; }
    return differences([...a], [...b], path, out);
  }
  if (a !== null && b !== null && typeof a === 'object' && typeof b === 'object') {
    const keys = new Set([...Object.keys(a), ...Object.keys(b)]);
    for (const key of keys) differences((a as Record<string, unknown>)[key], (b as Record<string, unknown>)[key], `${path}.${key}`, out);
    return out;
  }
  if (!Object.is(a, b)) out.push(`${path}: ${String(a)} vs ${String(b)}`);
  return out;
}

/** The files a finished scan is saved as, built with a fixed creation time. A scan too small to be saved gives the same refusal on both paths. */
function artifacts(snapshot: ScanPointCloudSnapshot) {
  const full = { ...snapshot, manualControlPoints: [], operatorEdges: [] } as never;
  const out: { scan: Uint8Array | null; occupancy: unknown; preview: unknown; error: string | null } = { scan: null, occupancy: null, preview: null, error: null };
  try {
    const encoded = encodeScanArtifactWithIdentity(full);
    out.scan = encoded.bytes;
    const occupancy = buildSessionOccupancyArtifact(full, encoded.identity, 1);
    out.occupancy = occupancy;
    out.preview = buildVoxelPreviewArtifact(full, encoded.identity, { occupancyArtifact: occupancy as never, identity: encoded.identity } as never);
  } catch (error) { out.error = error instanceof Error ? error.message : String(error); }
  return out;
}

const byteLengthOf = (value: unknown): number => (ArrayBuffer.isView(value) ? value.byteLength : value !== null && typeof value === 'object' && ArrayBuffer.isView((value as { bytes?: unknown }).bytes) ? (value as { bytes: Uint8Array }).bytes.byteLength : 0);

/*
 * The REAL worker against the synchronous accumulator on the same frames, every frame accepted (the next one is handed over when the previous is fused).
 * With `restartAt`, the worker is thrown away mid-scan and a replacement continues from the real keeper worker's checkpoint plus the journal.
 */
async function equivalence(pack: Pack, restartAt: number | null, corruptWorkerFrames = false) {
  /* Negative control: the worker (only) gets every tenth frame with its depth moved by 3 mm; the comparison has to report it. */
  const corrupted = (index: number): ReturnType<typeof deviceDepth> => {
    if (!corruptWorkerFrames || index % 10 !== 0) return deviceDepth(pack, index);
    const copy = pack.frames[index].depth.slice(0); const values = new Uint16Array(copy);
    for (let i = 0; i < values.length; i += 1) if (values[i] !== 0) values[i] += 3;
    return deviceDepth({ ...pack, frames: [{ ...pack.frames[index], depth: copy }] }, 0);
  };
  const accumulator = new DepthPointCloudAccumulator();
  const recoveryReports: unknown[] = [];
  const fusion = new RuntimeFusion({ workerFactory: createDepthFusionWorker, checkpointEveryFrames: 20, onRecovered: (report) => recoveryReports.push(report) });
  fusion.reset();
  if (fusion.mode !== 'worker') throw new Error('the fusion worker could not be started');
  let restarted: boolean | null = null; let checkpointsBeforeRestart = 0; let recoveryMs = 0;
  for (let i = 0; i < pack.frames.length; i += 1) {
    const view = { projectionMatrix: pack.projection, transform: { matrix: pack.frames[i].worldFromView } };
    accumulator.sampleDepthFrame(deviceDepth(pack, i), view, contextOf(pack.frames[i]));
    if (restartAt === i) { checkpointsBeforeRestart = fusion.client.counters.checkpointsStored; const t0 = performance.now(); restarted = fusion.client.restart('equivalence run: deliberate restart'); for (let wait = 0; recoveryReports.length === 0 && wait < 4000; wait += 1) await sleep(1); recoveryMs = performance.now() - t0; }
    for (let wait = 0; !fusion.client.canAccept(pack.frames[i].keyframe) && wait < 4000; wait += 1) await sleep(1);
    const outcome = fusion.submitFrame(corrupted(i), view, contextOf(pack.frames[i]), 'unsigned-short', performance.now());
    if (outcome !== 'accepted') throw new Error(`frame ${i} was not accepted: ${outcome}`);
    for (let wait = 0; fusion.client.queueDepth > 0 && wait < 4000; wait += 1) await sleep(0);
  }
  const frames = pack.frames.length;
  const expectedCoverage = structuredClone(accumulator.liveCoverageSnapshot(4000));
  const actualCoverage = await fusion.client.request('coverage', { budget: 4000 });
  const expectedMesh = structuredClone(accumulator.liveMeshInput(4000));
  const actualMesh = await fusion.client.request('liveMesh', { maxPoints: 4000 });
  const expectedStructural = structuredClone(accumulator.structuralWorkerSnapshot());
  const actualStructural = await fusion.client.request('structural');
  const expectedGuide = structuredClone(accumulator.structuralGuideSnapshot());
  const actualGuide = await fusion.client.request('structuralGuide');
  const result = await fusion.finish(frames, frames);
  const expected = accumulator.snapshot(frames, frames);
  const expectedArtifacts = artifacts(expected); const actualArtifacts = artifacts(result.snapshot);
  const cloneStarted = performance.now();
  structuredClone(captureFusionState(accumulator));
  const checkpointCloneMs = performance.now() - cloneStarted;
  const counters = fusion.client.counters;
  const arrays = Object.values(expected as unknown as Record<string, unknown>).filter((value) => ArrayBuffer.isView(value)) as ArrayBufferView[];
  const report = {
    frames, points: expected.pointCount, certified: expected.tsdfPointCount, occupied: expected.occupiedSurfacePositions.length / 3, free: expected.freeAirPositions.length / 3,
    comparedArrays: arrays.length, comparedBytes: arrays.reduce((sum, array) => sum + array.byteLength, 0),
    differences: {
      snapshot: differences(expected, result.snapshot), coverage: differences(expectedCoverage, actualCoverage), liveMeshInput: differences(expectedMesh, actualMesh),
      structural: differences(expectedStructural, actualStructural), certifiedStructural: differences(expectedGuide, actualGuide), artifacts: differences(expectedArtifacts, actualArtifacts),
    },
    artifactBytes: { scan: byteLengthOf(expectedArtifacts.scan), occupancy: byteLengthOf(expectedArtifacts.occupancy), preview: byteLengthOf(expectedArtifacts.preview), error: expectedArtifacts.error },
    recovery: restartAt === null ? null : { restartAt, restarted, checkpointsBeforeRestart, recoveryMs, reports: recoveryReports, lostFrames: counters.lostFrames, recoveries: counters.recoveries, replayedFrames: counters.replayedFrames },
    checkpoint: { stored: counters.checkpointsStored, finalStateCloneMs: checkpointCloneMs },
    maxJournal: counters.maxJournal,
  };
  fusion.dispose();
  return report;
}

(window as unknown as { __fusionBench: unknown }).__fusionBench = {
  equivalence: (pack: Pack, restartAt: number | null, corruptWorkerFrames?: boolean) => equivalence(pack, restartAt, corruptWorkerFrames),
  load: async (url: string) => parsePack(await (await fetch(url)).arrayBuffer()),
  run: async (pack: Pack, variant: 'sync' | 'worker', paceMs: number) => {
    const r = variant === 'sync' ? await runSync(pack, paceMs) : await runWorker(pack, paceMs);
    return { variant, frames: pack.frames.length, main: summarize(r.mainMs), latency: summarize(r.endToEndLatencyMs), finishMs: r.finishMs, workerMsTotal: r.workerMsTotal, mainMsTotal: r.mainMs.reduce((a, b) => a + b, 0), points: r.points, promoted: r.promoted, dropped: r.dropped, droppedKeyframes: r.droppedKeyframes, maxQueue: r.maxQueue, heapDeltaMb: r.heapDeltaMb };
  },
};
document.getElementById('state')!.textContent = 'ready';
