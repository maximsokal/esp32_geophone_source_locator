import type { DeviceComputeProfile } from './deviceComputeProfile';
import type { ScanPointCloudSnapshot } from './webxrRuntime';

const MIB = 1024 * 1024;
const MIN_TSDF_CHANNEL_SCRATCH_BYTES = 32 * MIB;
const MAX_TSDF_CHANNEL_SCRATCH_BYTES = 192 * MIB;
const TSDF_BYTES_PER_INPUT_POINT = 256;
const OPTIONAL_AI_MEMORY_RESERVATION_BYTES = 192 * MIB;
const MAX_TSDF_CHANNELS = 3;

export type PostScanWorkStage =
  | 'primary-durable-persistence'
  | 'lightweight-result-shell'
  | 'viewport-point-materialization'
  | 'tsdf-webgpu-reconstruction'
  | 'optional-ai';

export type PostScanStageStatus = 'idle' | 'active' | 'ready' | 'deferred';

export interface ScanSceneMaterializationNeeds {
  good: boolean;
  bad: boolean;
  freeAir: boolean;
  unknownSpace: boolean;
  manualPoints: boolean;
  dimensions: boolean;
}

export interface PostScanByteEstimate {
  originalSnapshotBytes: number;
  plannedViewportCopyBytes: number;
  plannedTsdfSliceBytes: number;
  tsdfScratchBytesPerChannel: number;
  workerConcurrency: number;
  plannedWorkerScratchBytes: number;
  totalPlannedBytes: number;
  optionalAiReservationBytes: number;
  totalWithOptionalAiBytes: number;
  optionalAiFitsMemory: boolean;
  memoryBudgetBytes: number;
  safeToOverlapPersistence: boolean;
  underMemoryPressure: boolean;
}

export interface PostScanAdmissionState {
  generation: string | number;
  evaluatedAtMs: number;
  persistenceBusy: boolean;
  stages: Record<PostScanWorkStage, PostScanStageStatus>;
  admitted: {
    viewportPointMaterialization: boolean;
    tsdfReconstruction: boolean;
    optionalAi: boolean;
  };
  deferredReason: string | null;
  bytes: PostScanByteEstimate;
}

export interface PostScanAdmissionInput {
  generation: string | number;
  snapshot: ScanPointCloudSnapshot;
  profile: DeviceComputeProfile;
  persistenceBusy: boolean;
  materialization: ScanSceneMaterializationNeeds;
  meshRequested: boolean;
  aiRequested: boolean;
  directionalChannelCount?: number;
}

function byteLength(value: unknown): number {
  return ArrayBuffer.isView(value) ? value.byteLength : 0;
}

/** Count owned typed arrays actually present on the scan snapshot. */
export function scanSnapshotArrayBytes(snapshot: ScanPointCloudSnapshot): number {
  let bytes = 0;
  for (const value of Object.values(snapshot)) bytes += byteLength(value);
  return bytes;
}

function viewportCopyBytes(snapshot: ScanPointCloudSnapshot, needs: ScanSceneMaterializationNeeds): number {
  let bytes = 0;
  if (needs.good) bytes += snapshot.positions.byteLength;
  if (needs.bad) bytes += snapshot.tentativePositions.byteLength;
  if (needs.freeAir) bytes += snapshot.freeAirPositions.byteLength;
  if (needs.unknownSpace) bytes += snapshot.unknownCandidatePositions.byteLength;
  if (needs.manualPoints) {
    bytes += snapshot.operatorEdges.length * 6 * Float32Array.BYTES_PER_ELEMENT;
    bytes += snapshot.manualControlPoints.length * 3 * Float32Array.BYTES_PER_ELEMENT;
  }
  if (needs.dimensions && snapshot.bounds !== null) bytes += 12 * 2 * 3 * Float32Array.BYTES_PER_ELEMENT;
  return bytes;
}

function tsdfSliceBytes(snapshot: ScanPointCloudSnapshot): number {
  return snapshot.tsdfPositions.byteLength +
    snapshot.tsdfNormals.byteLength +
    snapshot.tsdfCoverageConfidence.byteLength +
    snapshot.freeAirPositions.byteLength;
}

function tsdfScratchBytesPerChannel(snapshot: ScanPointCloudSnapshot): number {
  const inputPoints = Math.floor(snapshot.tsdfPositions.length / 3) +
    Math.floor(snapshot.freeAirPositions.length / 3);
  return Math.max(
    MIN_TSDF_CHANNEL_SCRATCH_BYTES,
    Math.min(MAX_TSDF_CHANNEL_SCRATCH_BYTES, inputPoints * TSDF_BYTES_PER_INPUT_POINT),
  );
}

export function evaluatePostScanAdmission(input: PostScanAdmissionInput, evaluatedAtMs = Date.now()): PostScanAdmissionState {
  const originalSnapshotBytes = scanSnapshotArrayBytes(input.snapshot);
  const plannedViewportCopyBytes = viewportCopyBytes(input.snapshot, input.materialization);
  const plannedTsdfSliceBytes = input.meshRequested ? tsdfSliceBytes(input.snapshot) : 0;
  const tsdfScratchBytes = input.meshRequested ? tsdfScratchBytesPerChannel(input.snapshot) : 0;
  const memoryBudgetBytes = input.profile.postScanMemoryBudgetBytes;
  const fixedWorkingSetBytes = originalSnapshotBytes + plannedViewportCopyBytes + plannedTsdfSliceBytes;
  const cpuWorkerCapacity = Math.max(1, input.profile.postScanWorkerBudget);
  const memoryWorkerCapacity = tsdfScratchBytes === 0
    ? 1
    : Math.max(1, Math.floor(Math.max(0, memoryBudgetBytes - fixedWorkingSetBytes) / tsdfScratchBytes));
  const channelCapacity = Math.max(1, Math.min(MAX_TSDF_CHANNELS, Math.floor(input.directionalChannelCount ?? MAX_TSDF_CHANNELS)));
  const workerConcurrency = input.meshRequested
    ? Math.max(1, Math.min(cpuWorkerCapacity, memoryWorkerCapacity, channelCapacity))
    : 0;
  const plannedWorkerScratchBytes = tsdfScratchBytes * workerConcurrency;
  const totalPlannedBytes = fixedWorkingSetBytes + plannedWorkerScratchBytes;
  const optionalAiReservationBytes = input.aiRequested ? OPTIONAL_AI_MEMORY_RESERVATION_BYTES : 0;
  const totalWithOptionalAiBytes = totalPlannedBytes + optionalAiReservationBytes;
  const optionalAiFitsMemory = totalWithOptionalAiBytes <= memoryBudgetBytes;
  const safeToOverlapPersistence = totalPlannedBytes <= memoryBudgetBytes;
  const underMemoryPressure = totalPlannedBytes > memoryBudgetBytes;
  const heavyAdmitted = !input.persistenceBusy || safeToOverlapPersistence;
  const aiAdmitted = input.aiRequested && !input.persistenceBusy && optionalAiFitsMemory;
  const stages: Record<PostScanWorkStage, PostScanStageStatus> = {
    'primary-durable-persistence': input.persistenceBusy ? 'active' : 'ready',
    'lightweight-result-shell': 'ready',
    'viewport-point-materialization': !hasMaterialization(input.materialization)
      ? 'idle'
      : heavyAdmitted ? 'active' : 'deferred',
    'tsdf-webgpu-reconstruction': !input.meshRequested
      ? 'idle'
      : heavyAdmitted ? 'active' : 'deferred',
    'optional-ai': !input.aiRequested ? 'idle' : aiAdmitted ? 'active' : 'deferred',
  };
  const deferredReason = input.persistenceBusy && !safeToOverlapPersistence
    ? 'primary-persistence-byte-budget'
    : input.aiRequested && !aiAdmitted
      ? input.persistenceBusy ? 'primary-persistence-priority' : 'post-scan-memory-pressure'
      : null;

  return {
    generation: input.generation,
    evaluatedAtMs,
    persistenceBusy: input.persistenceBusy,
    stages,
    admitted: {
      viewportPointMaterialization: hasMaterialization(input.materialization) && heavyAdmitted,
      tsdfReconstruction: input.meshRequested && heavyAdmitted,
      optionalAi: aiAdmitted,
    },
    deferredReason,
    bytes: {
      originalSnapshotBytes,
      plannedViewportCopyBytes,
      plannedTsdfSliceBytes,
      tsdfScratchBytesPerChannel: tsdfScratchBytes,
      workerConcurrency,
      plannedWorkerScratchBytes,
      totalPlannedBytes,
      optionalAiReservationBytes,
      totalWithOptionalAiBytes,
      optionalAiFitsMemory,
      memoryBudgetBytes,
      safeToOverlapPersistence,
      underMemoryPressure,
    },
  };
}

/** Keep large TSDF copies inside the point where demand and admission are both true. */
export function runAdmittedTsdfRequest<Input, Result>(
  requested: boolean,
  admitted: boolean,
  materialize: () => Input,
  start: (input: Input) => Result,
): Result | null {
  if (!requested || !admitted) return null;
  return start(materialize());
}

function hasMaterialization(needs: ScanSceneMaterializationNeeds): boolean {
  return needs.good || needs.bad || needs.freeAir || needs.unknownSpace || needs.manualPoints || needs.dimensions;
}

export class PostScanAdmissionCoordinator {
  private state: PostScanAdmissionState | null = null;

  constructor(
    private readonly clock: () => number = () => Date.now(),
    private readonly onDecision?: (state: PostScanAdmissionState) => void,
  ) {}

  evaluate(input: PostScanAdmissionInput): PostScanAdmissionState {
    const state = evaluatePostScanAdmission(input, this.clock());
    this.state = state;
    this.onDecision?.(state);
    return state;
  }

  getState(): PostScanAdmissionState | null {
    return this.state;
  }
}
