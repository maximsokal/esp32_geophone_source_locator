import { DepthPointCloudAccumulator, type MonocularSurfacePrior } from './depthPointCloud';
import { samplerFromDepthFrameBuffer } from './depthFusionFrame';
import {
  transferablesOf,
  type FusionFrameDone, type FusionFrameMessage, type FusionRequestMessage, type FusionResponse, type FusionStats, type FusionWorkerRequest, type FusionWorkerResponse,
} from './depthFusionProtocol';
import { latestRgbGuidance, setLatestRgbGuidance } from './rgbGuidanceStore';

/**
 * The state and the message handling of the depth-fusion worker, free of any Worker global so that the SAME code runs in the worker, in a test, and in
 * the inline fallback of the main thread (CPU fallback when no worker can be started).
 *
 * The accumulator is the production one. The monocular (AI) prior is staged by the main thread, which owns that singleton, so the worker's accumulator gets
 * a null prior; RGB guidance arrives with the frame it was matched to and is published to the guidance store just before that frame is fused, which is
 * exactly what the synchronous path read from the registry.
 */
const NO_PRIOR: MonocularSurfacePrior = { isEnabled: false, reset: () => undefined };

export type FusionPost = (message: FusionWorkerResponse, transfer?: ArrayBuffer[]) => void;

export class DepthFusionCore {
  readonly accumulator = new DepthPointCloudAccumulator(undefined, undefined, undefined, NO_PRIOR);
  private generation = 0;
  private integratedFrames = 0;

  constructor(private readonly clock: () => number = () => performance.now()) {}

  stats(): FusionStats {
    return { pointCount: this.accumulator.pointCount, promotedCount: this.accumulator.promotedCount, polishFrameCount: this.accumulator.polishFrameCount, keyframeCount: this.accumulator.keyframeCount, integratedFrames: this.integratedFrames };
  }

  reset(generation: number): void {
    this.generation = generation;
    this.integratedFrames = 0;
    setLatestRgbGuidance(null);
    this.accumulator.reset();
  }

  /** Fuses one frame. Frames of another generation are refused (the caller answers with an error the client ignores). */
  integrate(message: FusionFrameMessage): { changedPoints: number; workMs: number } {
    if (message.generation !== this.generation) throw new Error(`frame of generation ${message.generation} while generation ${this.generation} is current`);
    const started = this.clock();
    const sampler = samplerFromDepthFrameBuffer(message.frame, message.geometry);
    const previousGuidance = latestRgbGuidance();                 /* inline on the main thread this store is the runtime's own: put its frame back */
    setLatestRgbGuidance(message.guidance === null ? null : { ...message.guidance, capturedAtMs: started });
    try {
      const result = this.accumulator.sampleDepthFrame(sampler, { projectionMatrix: message.geometry.projectionMatrix, transform: { matrix: message.geometry.worldFromView } }, message.context);
      this.integratedFrames += 1;
      return { changedPoints: result.changedPoints, workMs: this.clock() - started };
    } finally {
      setLatestRgbGuidance(previousGuidance);
    }
  }

  answer(request: FusionRequestMessage): { payload: unknown; workMs: number } {
    if (request.generation !== this.generation) throw new Error(`request of generation ${request.generation} while generation ${this.generation} is current`);
    const started = this.clock();
    const params = request.params ?? {};
    let payload: unknown = null;
    switch (request.kind) {
      case 'coverage': payload = this.accumulator.liveCoverageSnapshot(params.budget); break;
      case 'liveMesh': payload = this.accumulator.liveMeshInput(params.maxPoints ?? 0); break;
      case 'structural': payload = this.accumulator.structuralWorkerSnapshot(); break;
      case 'structuralGuide': payload = this.accumulator.structuralGuideSnapshot(); break;
      case 'snapshot': payload = this.accumulator.snapshot(params.depthFrameCount ?? 0, params.sampledDepthFrames ?? 0); break;
      case 'stats': payload = this.stats(); break;
    }
    return { payload, workMs: this.clock() - started };
  }
}

/** The worker's message loop as a function: feed it requests, it posts responses (with the buffers it hands over). */
export function createDepthFusionHandler(post: FusionPost, core: DepthFusionCore = new DepthFusionCore()): { core: DepthFusionCore; handle: (message: FusionWorkerRequest) => void } {
  const handle = (message: FusionWorkerRequest): void => {
    try {
      if (message.type === 'reset') { core.reset(message.generation); return; }
      if (message.type === 'frame') {
        const { changedPoints, workMs } = core.integrate(message);
        const done: FusionFrameDone = { type: 'frameDone', generation: message.generation, seq: message.seq, buffer: message.frame.data, changedPoints, workMs, stats: core.stats() };
        post(done, [message.frame.data]);
        return;
      }
      const { payload, workMs } = core.answer(message);
      const response: FusionResponse = { type: 'response', generation: message.generation, id: message.id, kind: message.kind, payload, workMs, stats: core.stats() };
      post(response, transferablesOf(payload));
    } catch (error) {
      const reason = error instanceof Error ? error.message : String(error);
      if (message.type === 'frame') post({ type: 'error', generation: message.generation, seq: message.seq, message: reason }, [message.frame.data]);
      else if (message.type === 'request') post({ type: 'error', generation: message.generation, id: message.id, message: reason });
      else post({ type: 'error', generation: message.generation, message: reason });
    }
  };
  return { core, handle };
}
