import { clientDiagnostics } from '../data/clientDiagnostics';
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

interface WorkerResultMessage {
  type: 'result';
  requestId: number;
  epoch: number;
  result: StructuralGuideResult;
  diagnostics?: StructuralCandidateDiagnostics & { nativePlanePriors?: number; nativePlaneStabilizedSurfels?: number };
}

interface PendingAnalysis {
  id: number;
  epoch: number;
  resolve: (value: StructuralGuideResult) => void;
  reject: (reason?: unknown) => void;
  started: number;
}

export class StructuralGuideClient {
  private readonly worker: Worker;
  private nextRequestId = 1;
  private epoch = 0;
  private pending: PendingAnalysis | null = null;

  constructor() {
    // Vite inlines the worker payload into the application bundle. It still runs
    // in a dedicated browser thread, while the ESP firmware keeps the existing
    // single-JS-asset contract and needs no extra HTTP route.
    this.worker = new StructuralGuideWorker();
    this.worker.addEventListener('message', this.handleMessage);
    this.worker.addEventListener('error', this.handleError);
  }

  get busy(): boolean { return this.pending !== null; }

  analyze(input: AnalyzeInput): Promise<StructuralGuideResult> {
    if (this.pending !== null) return Promise.reject(new Error('Structural guide analysis is already running.'));
    const id = this.nextRequestId++;
    const epoch = this.epoch;
    // Callers may explicitly override priors for deterministic tests. Production
    // analyses otherwise consume the latest frame-owned numeric plane snapshot.
    // Native planes remain advisory: the worker can only rotate normals of
    // already-measured physical surfels and cannot create OCCUPIED/FREE geometry.
    const planePriors = input.planePriors ?? getLatestStructuralPlanePriors();
    return new Promise((resolve, reject) => {
      this.pending = { id, epoch, resolve, reject, started: performance.now() };
      const transfer: Transferable[] = [input.positions.buffer, input.normals.buffer, input.confidence.buffer];
      if (input.residualMeters !== undefined) transfer.push(input.residualMeters.buffer);
      if (input.normalConfidence !== undefined) transfer.push(input.normalConfidence.buffer);
      if (input.coverageConfidence !== undefined) transfer.push(input.coverageConfidence.buffer);
      if (input.coverageReady !== undefined) transfer.push(input.coverageReady.buffer);
      if (input.freeSpacePositions !== undefined) transfer.push(input.freeSpacePositions.buffer);
      this.worker.postMessage({
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
  }

  reset(): void {
    this.epoch += 1;
    if (this.pending !== null) {
      this.pending.reject(new Error('Structural guide analysis reset for a new scan.'));
      this.pending = null;
    }
    this.worker.postMessage({ type: 'reset', epoch: this.epoch });
  }

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
    if (event.data?.type !== 'result' || !isCurrentStructuralGuideWorkerResult(event.data, this.pending, this.epoch)) return;
    const pending = this.pending!;
    this.pending = null;
    const result = { ...event.data.result, computeMs: event.data.result.computeMs || (performance.now() - pending.started) };
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
