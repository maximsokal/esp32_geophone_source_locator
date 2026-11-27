import type { WebXrDepthDataFormat } from './cpuDepthSampler';
import { prepareCpuDepthSampler } from './cpuDepthSampler';
import type {
  CpuDepthInformationLike, DepthFusionFrameContext, DepthSampleView, LiveCoverageSnapshot, ScanPointCloudSnapshot, StructuralGuideSnapshot,
} from './depthPointCloud';
import { DepthFusionClient, type DepthFusionClientOptions, type SubmitOutcome } from './depthFusionClient';
import type { FusionGuidancePacket, FusionStats } from './depthFusionProtocol';
import { createDepthFusionKeeper, createDepthFusionWorker } from './depthFusionWorkerFactory';
import { FUSION_BASE_INTERVAL_MS, FusionCaptureGovernor, type FusionCaptureStep } from './fusionCaptureGovernor';
import type { FusionPoseLike, WebXrFusionKeyframeSelector } from './webxrFusionQuality';
import { registerManualPlacementResolver, solveManualPlacementOnStableSurface } from './manualPlacementSurface';
import { optionalMonocularPrior } from './monocularPriorRuntime';
import { rgbGuidanceMatchesView } from './rgbDepthGuidance';
import { getFreshRgbGuidance } from './rgbGuidanceStore';

/**
 * What the XR runtime needs from the depth fusion, with the accumulated state owned by a worker (Phase 2). The runtime keeps the XR-frame-bound work
 * (RGB-guidance matching, the monocular staging that needs the live depth buffer, the keyframe decision) and hands everything else over; the answers
 * (coverage points, live-mesh input, structural snapshots, the final snapshot) come back asynchronously and are cached for the synchronous readers.
 * Every asynchronous answer is tied to the scan generation it was asked in: a late one is dropped.
 */
export interface RuntimeFusionOptions extends Omit<DepthFusionClientOptions, 'workerFactory'> {
  workerFactory?: () => Worker | null;
}

export interface FinishResult {
  snapshot: ScanPointCloudSnapshot;
  drained: boolean;
  outstandingFrames: number;
  outstandingKeyframes: number;
}

const GUIDE_REFRESH_MIN_MS = 1000;

/** What a pin preview sees before the first certified snapshot has arrived: no surface, so the placement is refused with its usual reason. */
export const EMPTY_STRUCTURAL_GUIDE: StructuralGuideSnapshot = {
  positions: new Float32Array(0), normals: new Float32Array(0), confidence: new Float32Array(0), pointCount: 0, keyframeCount: 0, voxelSizeMeters: 0.04,
  freeSpacePositions: new Float32Array(0), freeSpaceVoxelSizeMeters: 0.04,
};

export class RuntimeFusion {
  private clientInstance: DepthFusionClient;
  readonly governor = new FusionCaptureGovernor();
  private parked = false;
  private readonly options: RuntimeFusionOptions;
  private coverage: LiveCoverageSnapshot | null = null;
  private coverageFetching = false;
  private liveMeshInput: { positions: Float32Array; normals: Float32Array; confidence: Float32Array; voxelSizeMeters: number } | null = null;
  private liveMeshFetching = false;
  private guideMirror: StructuralGuideSnapshot | null = null;
  private guideFetching: Promise<StructuralGuideSnapshot> | null = null;
  private guideFetchedAt = Number.NEGATIVE_INFINITY;

  constructor(options: RuntimeFusionOptions = {}) {
    this.options = options;
    this.clientInstance = this.createClient();
  }

  private createClient(): DepthFusionClient {
    const { workerFactory, ...rest } = this.options;
    const client: DepthFusionClient = new DepthFusionClient({
      keeperFactory: createDepthFusionKeeper,
      ...rest,
      workerFactory: workerFactory ?? createDepthFusionWorker,
      onFrameTiming: (workMs, queueDepth) => {
        if (client !== undefined) this.governor.frameDone(workMs, { ...client.limits, queueDepth });
        rest.onFrameTiming?.(workMs, queueDepth);
      },
    });
    if (client.mode === 'worker') {
      /* In worker mode the manual pin resolver reads the mirror of the certified structural snapshot (at most a second old); inline, the accumulator registers its own. */
      registerManualPlacementResolver((candidate) => solveManualPlacementOnStableSurface(candidate, this.guideMirror ?? EMPTY_STRUCTURAL_GUIDE));
    }
    return client;
  }

  get client(): DepthFusionClient { return this.clientInstance; }

  get mode(): 'worker' | 'inline' { return this.client.mode; }
  get stats(): FusionStats { return this.client.stats; }
  get promotedCount(): number { return this.client.stats.promotedCount; }
  get pointCount(): number { return this.client.stats.pointCount; }
  get keyframeCount(): number { return this.client.stats.keyframeCount; }
  get polishFrameCount(): number { return this.client.stats.polishFrameCount; }

  /** A new scan: every cache and every answer still on its way belongs to the old one. */
  reset(): void {
    this.coverage = null; this.coverageFetching = false;
    this.liveMeshInput = null; this.liveMeshFetching = false;
    this.guideMirror = null; this.guideFetching = null; this.guideFetchedAt = Number.NEGATIVE_INFINITY;
    this.governor.reset();
    if (this.parked) { this.clientInstance = this.createClient(); this.parked = false; }
    this.clientInstance.reset();
  }

  /** The scan's session is over and its snapshot taken: the worker and everything it holds are released; the next reset() starts a fresh one. */
  park(): void {
    if (this.parked) return;
    this.parked = true;
    this.clientInstance.dispose();
  }

  /** Admission and rate control of one XR frame, BEFORE any depth is captured (see {@link FusionCaptureGovernor}). */
  captureStep(timeMs: number, pose: FusionPoseLike, selector: WebXrFusionKeyframeSelector): FusionCaptureStep {
    return this.governor.step(timeMs, pose, selector, this.client.limits);
  }

  /** True while not even a new viewpoint can be taken: the operator is asked to hold still. */
  get catchingUp(): boolean { return this.governor.catchingUp; }

  /** True while the fusion cannot keep its normal rate: the tasks that share its worker or the CPU yield to it. */
  get behind(): boolean { return this.governor.catchingUp || this.governor.intervalMs > FUSION_BASE_INTERVAL_MS * 1.5; }

  /**
   * Hands one XR depth frame to the fusion. Called inside the frame callback: the device buffer is COPIED here and nothing of the XR frame leaves it.
   * RGB guidance is matched exactly as the synchronous path matched it, and the monocular (AI) staging, which needs the live depth, is done here.
   */
  submitFrame(depth: CpuDepthInformationLike, view: DepthSampleView, context: DepthFusionFrameContext, dataFormat: WebXrDepthDataFormat, nowMs: number): SubmitOutcome {
    const geometry = { projectionMatrix: Float32Array.from(view.projectionMatrix as ArrayLike<number>), worldFromView: Float32Array.from(view.transform.matrix as ArrayLike<number>) };
    const fresh = getFreshRgbGuidance(nowMs);
    const guidance = fresh !== null && rgbGuidanceMatchesView(fresh, geometry.worldFromView) ? fresh : null;
    if (context.evidenceKeyframe && guidance !== null && optionalMonocularPrior.isEnabled) optionalMonocularPrior.stagePostScan?.(guidance, prepareCpuDepthSampler(depth, dataFormat).sampler);
    const packet: FusionGuidancePacket | null = guidance === null ? null : {
      width: guidance.width, height: guidance.height, luma: guidance.luma, edgeStrength: guidance.edgeStrength, gradientX: guidance.gradientX, gradientY: guidance.gradientY,
      ...(guidance.projectionMatrix === undefined ? {} : { projectionMatrix: guidance.projectionMatrix }),
      ...(guidance.worldFromView === undefined ? {} : { worldFromView: guidance.worldFromView }),
    };
    return this.client.submit({ depth, dataFormat, geometry, context, guidance: packet, mandatory: context.evidenceKeyframe });
  }

  /** Asks for a coverage snapshot unless one is on its way; the answer is picked up with {@link takeCoverage}. */
  requestCoverage(budget: number): void {
    if (this.coverageFetching || this.client.hasFailed !== null) return;
    this.coverageFetching = true;
    const generation = this.client.currentGeneration;
    this.client.request<LiveCoverageSnapshot>('coverage', { budget }).then((snapshot) => {
      this.coverage = snapshot;
    }, () => undefined).finally(() => { if (generation === this.client.currentGeneration) this.coverageFetching = false; });
  }

  takeCoverage(): LiveCoverageSnapshot | null { const value = this.coverage; this.coverage = null; return value; }

  /** Live-mesh input, same pattern: ask when none is held or on its way, take it when the mesh client is ready for it. */
  requestLiveMeshInput(maxPoints: number): void {
    if (this.liveMeshFetching || this.liveMeshInput !== null || this.client.hasFailed !== null) return;
    this.liveMeshFetching = true;
    const generation = this.client.currentGeneration;
    this.client.request<RuntimeFusion['liveMeshInput']>('liveMesh', { maxPoints }).then((input) => {
      if (generation === this.client.currentGeneration) this.liveMeshInput = input;
    }, () => undefined).finally(() => { if (generation === this.client.currentGeneration) this.liveMeshFetching = false; });
  }

  takeLiveMeshInput(): RuntimeFusion['liveMeshInput'] { const value = this.liveMeshInput; this.liveMeshInput = null; return value; }

  requestStructuralWorkerSnapshot(): Promise<StructuralGuideSnapshot> { return this.client.request<StructuralGuideSnapshot>('structural'); }

  /** The certified structural snapshot, fresh from the owner of the state (also refreshes the mirror the manual pin resolver reads). */
  requestStructuralGuideSnapshot(): Promise<StructuralGuideSnapshot> {
    const generation = this.client.currentGeneration;
    return this.client.request<StructuralGuideSnapshot>('structuralGuide').then((snapshot) => {
      if (generation === this.client.currentGeneration) { this.guideMirror = snapshot; this.guideFetchedAt = performance.now(); }
      return snapshot;
    });
  }

  /** What the synchronous readers (manual pin previews) get: the latest certified snapshot, refreshed in the background at most once a second. */
  structuralGuideMirror(): StructuralGuideSnapshot | null {
    const inline = this.client.inlineCore;
    if (inline !== null) return inline.accumulator.structuralGuideSnapshot();
    if (this.guideFetching === null && performance.now() - this.guideFetchedAt >= GUIDE_REFRESH_MIN_MS) {
      this.guideFetching = this.requestStructuralGuideSnapshot().catch(() => this.guideMirror as StructuralGuideSnapshot).finally(() => { this.guideFetching = null; });
    }
    return this.guideMirror;
  }

  /** Inline only: a synchronous snapshot (the worker path has none; it must use {@link finish}). */
  inlineSnapshot(depthFrames: number, sampledFrames: number): ScanPointCloudSnapshot | null {
    return this.client.inlineCore?.accumulator.snapshot(depthFrames, sampledFrames) ?? null;
  }

  /**
   * Finish: stop taking frames, let the accepted ones finish (bounded), then take the snapshot. A worker that does not answer in time raises an error
   * the caller reports; the scan is not silently replaced by an empty one.
   */
  async finish(depthFrames: number, sampledFrames: number): Promise<FinishResult> {
    const attempt = async (): Promise<FinishResult> => {
      const drain = await this.client.drain();
      if (!drain.drained) throw new Error(`depth fusion did not finish ${drain.outstandingFrames} accepted frames (${drain.outstandingKeyframes} keyframes) in time`);
      const snapshot = await this.client.request<ScanPointCloudSnapshot>('snapshot', { depthFrameCount: depthFrames, sampledDepthFrames: sampledFrames });
      return { snapshot, drained: true, outstandingFrames: 0, outstandingKeyframes: 0 };
    };
    try {
      return await attempt();
    } catch (error) {
      /* A worker that stopped answering is replaced (checkpoint + journal) and asked once more; if that is not possible the error stands. */
      if (!this.client.restart(`finish: ${error instanceof Error ? error.message : String(error)}`)) throw error;
      this.client.resumeIntake();
      return attempt();
    }
  }

  dispose(): void { this.parked = true; this.clientInstance.dispose(); }
}
