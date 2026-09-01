import type { StructuralGuideResult } from './structuralGuide';

interface AnalyzeInput {
  positions: Float32Array;
  normals: Float32Array;
  confidence: Float32Array;
  keyframeCount: number;
  voxelSizeMeters: number;
}

interface WorkerResultMessage {
  type: 'result';
  requestId: number;
  result: StructuralGuideResult;
}

export class StructuralGuideClient {
  private readonly worker: Worker;
  private nextRequestId = 1;
  private pending: { id: number; resolve: (value: StructuralGuideResult) => void; reject: (reason?: unknown) => void; started: number } | null = null;

  constructor() {
    this.worker = new Worker(new URL('./structuralGuide.worker.ts', import.meta.url), { type: 'module' });
    this.worker.addEventListener('message', this.handleMessage);
    this.worker.addEventListener('error', this.handleError);
  }

  get busy(): boolean { return this.pending !== null; }

  analyze(input: AnalyzeInput): Promise<StructuralGuideResult> {
    if (this.pending !== null) return Promise.reject(new Error('Structural guide analysis is already running.'));
    const id = this.nextRequestId++;
    return new Promise((resolve, reject) => {
      this.pending = { id, resolve, reject, started: performance.now() };
      this.worker.postMessage({
        type: 'analyze',
        requestId: id,
        positions: input.positions,
        normals: input.normals,
        confidence: input.confidence,
        keyframeCount: input.keyframeCount,
        voxelSizeMeters: input.voxelSizeMeters,
      }, [input.positions.buffer, input.normals.buffer, input.confidence.buffer]);
    });
  }

  reset(): void { this.worker.postMessage({ type: 'reset' }); }

  dispose(): void {
    if (this.pending !== null) {
      this.pending.reject(new Error('Structural guide worker disposed.'));
      this.pending = null;
    }
    this.worker.removeEventListener('message', this.handleMessage);
    this.worker.removeEventListener('error', this.handleError);
    this.worker.terminate();
  }

  private readonly handleMessage = (event: MessageEvent<WorkerResultMessage>): void => {
    if (event.data?.type !== 'result' || this.pending === null || event.data.requestId !== this.pending.id) return;
    const pending = this.pending;
    this.pending = null;
    pending.resolve({ ...event.data.result, computeMs: event.data.result.computeMs || (performance.now() - pending.started) });
  };

  private readonly handleError = (event: ErrorEvent): void => {
    if (this.pending !== null) {
      this.pending.reject(event.error ?? new Error(event.message));
      this.pending = null;
    }
  };
}
