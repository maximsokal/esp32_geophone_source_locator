import { DepthPointCloudAccumulator } from '../src/xr/depthPointCloud';
import type { DepthFusionFrameContext } from '../src/xr/depthPointCloudV2';
import { RuntimeFusion } from '../src/xr/depthFusionRuntime';
import { createDepthFusionWorker } from '../src/xr/depthFusionWorkerFactory';

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

(window as unknown as { __fusionBench: unknown }).__fusionBench = {
  load: async (url: string) => parsePack(await (await fetch(url)).arrayBuffer()),
  run: async (pack: Pack, variant: 'sync' | 'worker', paceMs: number) => {
    const r = variant === 'sync' ? await runSync(pack, paceMs) : await runWorker(pack, paceMs);
    return { variant, frames: pack.frames.length, main: summarize(r.mainMs), latency: summarize(r.endToEndLatencyMs), finishMs: r.finishMs, workerMsTotal: r.workerMsTotal, mainMsTotal: r.mainMs.reduce((a, b) => a + b, 0), points: r.points, promoted: r.promoted, dropped: r.dropped, droppedKeyframes: r.droppedKeyframes, maxQueue: r.maxQueue, heapDeltaMb: r.heapDeltaMb };
  },
};
document.getElementById('state')!.textContent = 'ready';
