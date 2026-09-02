import { clientDiagnostics } from '../data/clientDiagnostics';
import { liveComputeScheduler } from './liveComputeScheduler';
import type { SurfaceComponentReadinessSummary } from './scanCapabilityReadiness';
import type { StructuralCandidateDiagnostics } from './structuralCandidatePolicy';
import StructuralGuideWorker from './structuralGuide.worker?worker&inline';
import type { StructuralGuideResult, StructuralGuideViewpoint } from './structuralGuide';
import { isCurrentStructuralGuideWorkerResult } from './structuralGuideProtocol';
import type { StructuralPlanePrior } from './webxrPlaneGuidance';
import { getLatestStructuralPlanePriors } from './xrPlaneGuidanceRegistry';

interface AnalyzeInput {
  positions: Float32Array;
  normals: Float32Array;
  confidence: Float32Array;
  residualMeters?: Float32Array;
  normalConfidence?: Float32Array;
  coverageConfidence?: Float32Array;
  coverageReady?: Uint8Array;
  keyframeCount: number;
  voxelSizeMeters: number;
  viewpoint?: StructuralGuideViewpoint | null;
  freeSpacePositions?: Float32Array;
  freeSpaceVoxelSizeMeters?: number;
  /** Small structured-cloned advisory list; never transferred as physical geometry. */
  planePriors?: readonly StructuralPlanePrior[];
}

export interface StructuralGuideWorkerResult extends StructuralGuideResult {
  /** Compact component metrics computed on the worker; no point masks are returned. */
  surfaceReadiness?: SurfaceComponentReadinessSummary;
}

interface WorkerResultMessage {
  type: 'result';
  requestId: number;
  epoch: number;
  result: StructuralGuideWorkerResult;
  diagnostics?: StructuralCandidateDiagnostics & { nativePlanePriors?: number; nativePlaneStabilizedSurfels?: number };
}

interface PendingAnalysis {
  id: number;
  epoch: number;
  resolve: (value: StructuralGuideWorkerResult) => void;
  reject: (reason?: unknown) => void;
  started: number;
}

export class StructuralGuideClient {
  private worker: Worker | null = null;
  private nextRequestId = 1;
  private epoch = 0;
  private pending: PendingAnalysis | null = null;
  private starting = false;
  private disposed = false;

  get busy(): boolean { return this.pending !== null || this.starting; }

  private ensureWorker(): Worker {
    if (this.disposed) throw new Error('Structural guide worker disposed.');
    if (this.worker !== null) return this.worker;
    // Vite inlines the payload into the single ESP WebUI JS asset, but the code
    // still executes in a Dedicated Worker scheduled independently of the XR/UI
    // main thread. Creation is deliberately lazy: a tiny scan never pays for it.
    const worker = new StructuralGuideWorker();
    worker.addEventListener('message', this.handleMessage);
    worker.addEventListener('error', this.handleError);
    this.worker = worker;
    return worker;
  }

  async analyze(input: AnalyzeInput): Promise<StructuralGuideWorkerResult> {
    if (this.busy) throw new Error('Structural guide analysis is already running.');
    if (this.disposed) throw new Error('Structural guide worker disposed.');
    this.starting = true;
    const lease = await liveComputeScheduler.tryAcquire('structural');
    if (lease === null) {
      this.starting = false;
      throw new Error('Live compute budget is busy; structural analysis will retry on a later keyframe.');
    }
    try {
      const worker = this.ensureWorker();
      const id = this.nextRequestId++;
      const epoch = this.epoch;
      // Callers may explicitly override priors for deterministic tests. Production
      // analyses otherwise consume the latest frame-owned numeric plane snapshot.
      // Native planes remain advisory: the worker can only rotate normals of
      // already-measured physical surfels and cannot create OCCUPIED/FREE geometry.
      const planePriors = input.planePriors ?? getLatestStructuralPlanePriors();
      return await new Promise<StructuralGuideWorkerResult>((resolve, reject) => {
        this.pending = { id, epoch, resolve, reject, started: performance.now() };
        const transfer: Transferable[] = [input.positions.buffer, input.normals.buffer, input.confidence.buffer];
        if (input.residualMeters !== undefined) transfer.push(input.residualMeters.buffer);
        if (input.normalConfidence !== undefined) transfer.push(input.normalConfidence.buffer);
        if (input.coverageConfidence !== undefined) transfer.push(input.coverageConfidence.buffer);
        if (input.coverageReady !== undefined) transfer.push(input.coverageReady.buffer);
        if (input.freeSpacePositions !== undefined) transfer.push(input.freeSpacePositions.buffer);
        worker.postMessage({
          type: 'analyze',
          requestId: id,
          epoch,
          positions: input.positions,
          normals: input.normals,
          confidence: input.confidence,
          residualMeters: input.residualMeters,
          normalConfidence: input.normalConfidence,
          coverageConfidence: input.coverageConfidence,
          coverageReady: input.coverageReady,
          keyframeCount: input.keyframeCount,
          voxelSizeMeters: input.voxelSizeMeters,
          viewpoint: input.viewpoint ?? null,
          freeSpacePositions: input.freeSpacePositions,
          freeSpaceVoxelSizeMeters: input.freeSpaceVoxelSizeMeters,
          planePriors,
        }, transfer);
      });
    } finally {
      lease.release();
      this.starting = false;
    }
  }

  reset(): void {
    this.epoch += 1;
    if (this.pending !== null) {
      this.pending.reject(new Error('Structural guide analysis reset for a new scan.'));
      this.pending = null;
    }
    this.worker?.postMessage({ type: 'reset', epoch: this.epoch });
  }

  dispose(): void {
    this.disposed = true;
    if (this.pending !== null) {
      this.pending.reject(new Error('Structural guide worker disposed.'));
      this.pending = null;
    }
    if (this.worker !== null) {
      this.worker.removeEventListener('message', this.handleMessage);
      this.worker.removeEventListener('error', this.handleError);
      this.worker.terminate();
      this.worker = null;
    }
  }

  private readonly handleMessage = (event: MessageEvent<WorkerResultMessage>): void => {
    if (event.data?.type !== 'result' || !isCurrentStructuralGuideWorkerResult(event.data, this.pending, this.epoch)) return;
    const pending = this.pending!;
    this.pending = null;
    const result: StructuralGuideWorkerResult = {
      ...event.data.result,
      computeMs: event.data.result.computeMs || (performance.now() - pending.started),
    };
    const diagnostics = event.data.diagnostics;
    if (diagnostics !== undefined) {
      clientDiagnostics.info('XR_GUIDE_CANDIDATE_MATURITY', diagnostics);
    } else {
      // Backward-compatible fallback for a stale worker payload during dev/HMR.
      const edgeConfidences = result.edges.map((edge) => edge.confidence);
      const cornerConfidences = result.corners.map((corner) => corner.confidence);
      clientDiagnostics.info('XR_GUIDE_CANDIDATE_MATURITY', {
        rawTrackedEdges: result.edges.length,
        operationalEdges: result.edges.length,
        rawEdgeMinConfidence: edgeConfidences.length > 0 ? Number(Math.min(...edgeConfidences).toFixed(3)) : null,
        rawEdgeMaxConfidence: edgeConfidences.length > 0 ? Number(Math.max(...edgeConfidences).toFixed(3)) : null,
        operationalEdgeMinConfidence: edgeConfidences.length > 0 ? Number(Math.min(...edgeConfidences).toFixed(3)) : null,
        operationalEdgeMaxConfidence: edgeConfidences.length > 0 ? Number(Math.max(...edgeConfidences).toFixed(3)) : null,
        edgeMaxObservations: result.edges.reduce((maximum, edge) => Math.max(maximum, edge.observations), 0),
        edgeMaxGeometryConfidence: result.edges.length > 0
          ? Number(Math.max(...result.edges.map((edge) => edge.geometryConfidence)).toFixed(3))
          : null,
        threePlaneCorners: result.corners.filter((corner) => !corner.id.startsWith('endpoint:')).length,
        endpointCorners: result.corners.filter((corner) => corner.id.startsWith('endpoint:')).length,
        cornerMaxConfidence: cornerConfidences.length > 0 ? Number(Math.max(...cornerConfidences).toFixed(3)) : null,
      });
    }
    pending.resolve(result);
  };

  private readonly handleError = (event: ErrorEvent): void => {
    if (this.pending !== null) {
      this.pending.reject(event.error ?? new Error(event.message));
      this.pending = null;
    }
  };
}
