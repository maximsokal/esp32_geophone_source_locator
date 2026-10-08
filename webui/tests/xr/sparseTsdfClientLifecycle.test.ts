import { describe, expect, it } from 'vitest';

import { assessMeshReconstructionPreflight } from '../../src/xr/meshReconstructionPreflight';
import { buildTsdfRequestArgs } from '../../src/components/ScanPointCloudViewport';
import { runAdmittedTsdfRequest } from '../../src/xr/postScanAdmission';
import { evaluatePostScanAdmission } from '../../src/xr/postScanAdmission';
import { deriveDeviceComputeProfile } from '../../src/xr/deviceComputeProfile';
import type { ScanPointCloudSnapshot } from '../../src/xr/webxrRuntime';
import { SparseTsdfMeshClient, type SparseTsdfReconstructInput } from '../../src/xr/sparseTsdfClient';
import type { SparseTsdfMesh } from '../../src/xr/sparseTsdf';

function gridInput(width = 9, height = 9, spacing = 0.04): SparseTsdfReconstructInput {
  const positions: number[] = [];
  const normals: number[] = [];
  const confidence: number[] = [];
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      positions.push(x * spacing, y * spacing, 0);
      normals.push(0, 0, 1);
      confidence.push(0.92);
    }
  }
  return {
    positions: Float32Array.from(positions),
    normals: Float32Array.from(normals),
    confidence: Float32Array.from(confidence),
    voxelSizeMeters: 0.04,
  };
}

function result(sourcePointCount: number): SparseTsdfMesh {
  return {
    trianglePositions: new Float32Array(0),
    triangleCount: 0,
    rawTriangleCount: 0,
    quarantinedTriangleCount: 0,
    topologyDiscardedTriangleCount: 0,
    nonManifoldEdgeCount: 0,
    nonManifoldRepairTriangleCount: 0,
    orientationRepairCount: 0,
    orientationConflictCount: 0,
    meshTopologyComponentCount: 0,
    selfIntersectionPairCount: 0,
    selfIntersectionAffectedTriangleCount: 0,
    selfIntersectionExactTestedPairs: 0,
    selfIntersectionBroadPhaseCandidatePairs: 0,
    selfIntersectionTruncated: false,
    selfIntersectionOversizedTriangleCount: 0,
    localNoiseHotspotCount: 0,
    localNoiseHotspotTriangleCount: 0,
    meshNoiseScore: 0,
    sourcePointCount,
    integratedPointCount: sourcePointCount,
    skippedLowConfidence: 0,
    rejectedComponentPoints: 0,
    componentCount: 1,
    eligibleComponentCount: 1,
    meshReadiness: 1,
    nodeCount: 1,
    voxelSizeMeters: 0.046,
    truncationMeters: 0.1265,
    computeMs: 1,
  };
}

class FakePreflightWorker {
  private readonly messageListeners = new Set<(event: MessageEvent) => void>();

  addEventListener(type: string, listener: EventListenerOrEventListenerObject): void {
    if (type === 'message' && typeof listener === 'function') {
      this.messageListeners.add(listener as (event: MessageEvent) => void);
    }
  }

  removeEventListener(type: string, listener: EventListenerOrEventListenerObject): void {
    if (type === 'message' && typeof listener === 'function') {
      this.messageListeners.delete(listener as (event: MessageEvent) => void);
    }
  }

  postMessage(message: Record<string, unknown>): void {
    if (message.type !== 'preflight') return;
    const positions = message.positions as Float32Array;
    const normals = message.normals as Float32Array;
    const confidence = message.confidence as Float32Array;
    const result = assessMeshReconstructionPreflight({
      positions,
      normals,
      confidence,
      voxelSizeMeters: Number(message.voxelSizeMeters),
    });
    const response = {
      type: 'preflight-result',
      requestId: Number(message.requestId),
      positions,
      normals,
      confidence,
      result,
      computeMs: 0.5,
    };
    queueMicrotask(() => {
      const event = { data: response } as MessageEvent;
      for (const listener of this.messageListeners) listener(event);
    });
  }

  terminate(): void {}
}

class FakeWorker {
  readonly messages: Array<Record<string, unknown>> = [];
  private epoch = 0;
  private readonly messageListeners = new Set<(event: MessageEvent) => void>();
  private readonly errorListeners = new Set<(event: ErrorEvent) => void>();

  addEventListener(type: string, listener: EventListenerOrEventListenerObject): void {
    if (typeof listener !== 'function') return;
    if (type === 'message') this.messageListeners.add(listener as (event: MessageEvent) => void);
    if (type === 'error') this.errorListeners.add(listener as (event: ErrorEvent) => void);
  }

  removeEventListener(type: string, listener: EventListenerOrEventListenerObject): void {
    if (typeof listener !== 'function') return;
    if (type === 'message') this.messageListeners.delete(listener as (event: MessageEvent) => void);
    if (type === 'error') this.errorListeners.delete(listener as (event: ErrorEvent) => void);
  }

  postMessage(message: Record<string, unknown>): void {
    this.messages.push(message);
    if (message.type === 'reset') {
      this.epoch = Number(message.epoch);
      return;
    }
    if (message.type !== 'reconstruct') return;
    const positions = message.positions as Float32Array;
    const response = {
      type: 'result',
      requestId: Number(message.requestId),
      epoch: this.epoch,
      result: result(Math.floor(positions.length / 3)),
    };
    queueMicrotask(() => {
      const event = { data: response } as MessageEvent;
      for (const listener of this.messageListeners) listener(event);
    });
  }

  terminated = false;

  terminate(): void { this.terminated = true; }
}

function postScanSnapshot(input: SparseTsdfReconstructInput): ScanPointCloudSnapshot {
  const pointCount = Math.floor(input.positions.length / 3);
  return {
    positions: input.positions,
    confidence: input.confidence,
    normals: input.normals,
    tentativePositions: new Float32Array(0),
    tentativeConfidence: new Float32Array(0),
    freeAirPositions: new Float32Array(0),
    unknownCandidatePositions: new Float32Array(0),
    tsdfPositions: input.positions,
    tsdfNormals: input.normals,
    tsdfCoverageConfidence: input.confidence,
    tsdfPointCount: pointCount,
    voxelSizeMeters: input.voxelSizeMeters,
    occupancyVoxelSizeMeters: 0.1,
    operatorEdges: [],
    manualControlPoints: [],
    bounds: null,
  } as unknown as ScanPointCloudSnapshot;
}

function testComputeProfile(deviceMemoryGiB: number) {
  return deriveDeviceComputeProfile({
    reportedLogicalProcessors: 8,
    reportedDeviceMemoryGiB: deviceMemoryGiB,
    workerApi: true,
    crossOriginIsolated: false,
    sharedArrayBufferApi: false,
    wasmApi: true,
    wasmSharedMemory: false,
    webGpuApi: false,
    webGpuAdapter: false,
  });
}

class HoldingWorker {
  readonly messages: Array<Record<string, unknown>> = [];
  terminated = false;
  private readonly messageListeners = new Set<(event: MessageEvent) => void>();
  private readonly errorListeners = new Set<(event: ErrorEvent) => void>();
  private resolveReconstructPosted!: () => void;
  readonly reconstructPosted = new Promise<void>((resolve) => { this.resolveReconstructPosted = resolve; });

  addEventListener(type: string, listener: EventListenerOrEventListenerObject): void {
    if (typeof listener !== 'function') return;
    if (type === 'message') this.messageListeners.add(listener as (event: MessageEvent) => void);
    if (type === 'error') this.errorListeners.add(listener as (event: ErrorEvent) => void);
  }

  removeEventListener(type: string, listener: EventListenerOrEventListenerObject): void {
    if (typeof listener !== 'function') return;
    if (type === 'message') this.messageListeners.delete(listener as (event: MessageEvent) => void);
    if (type === 'error') this.errorListeners.delete(listener as (event: ErrorEvent) => void);
  }

  postMessage(message: Record<string, unknown>): void {
    this.messages.push(message);
    if (message.type === 'reconstruct') this.resolveReconstructPosted();
  }

  terminate(): void { this.terminated = true; }
}

const preflightFactory = (): Worker => new FakePreflightWorker() as unknown as Worker;

describe('SparseTsdfMeshClient lazy worker lifecycle', () => {
  it('does not clone TSDF arrays or create a worker while mesh is not requested, then starts when admitted', async () => {
    const input = gridInput();
    const snapshot = {
      tsdfPointCount: 81,
      tsdfPositions: input.positions,
      tsdfNormals: input.normals,
      tsdfCoverageConfidence: input.confidence,
      voxelSizeMeters: input.voxelSizeMeters,
      freeAirPositions: new Float32Array(0),
      occupancyVoxelSizeMeters: 0.1,
    } as Parameters<typeof buildTsdfRequestArgs>[0];
    let factoryCalls = 0;
    const worker = new FakeWorker();
    const client = new SparseTsdfMeshClient(() => {
      factoryCalls += 1;
      return worker as unknown as Worker;
    }, preflightFactory);

    const hidden = runAdmittedTsdfRequest(false, true, () => buildTsdfRequestArgs(snapshot), (args) => client.reconstruct(args));
    expect(hidden).toBeNull();
    expect(factoryCalls).toBe(0);
    expect(snapshot.tsdfPositions.byteLength).toBe(input.positions.byteLength);

    const admitted = runAdmittedTsdfRequest(true, true, () => buildTsdfRequestArgs(snapshot), (args) => client.reconstruct(args));
    expect(admitted).not.toBeNull();
    await admitted;
    expect(factoryCalls).toBe(1);
    client.dispose();
  });

  it('keeps a large scan out of worker dispatch during persistence and permits a small overlap', async () => {
    const largeCount = 500_000;
    const largeScan = {
      ...postScanSnapshot(gridInput(2, 2)),
      positions: new Float32Array(largeCount * 3),
      confidence: new Float32Array(largeCount),
      normals: new Float32Array(largeCount * 3),
      freeAirPositions: new Float32Array(largeCount * 3),
      tsdfPositions: new Float32Array(largeCount * 3),
      tsdfNormals: new Float32Array(largeCount * 3),
      tsdfCoverageConfidence: new Float32Array(largeCount),
      tsdfPointCount: largeCount,
    } as unknown as ScanPointCloudSnapshot;
    const profile = testComputeProfile(1);
    const materialization = {
      good: true, bad: false, freeAir: false, unknownSpace: false, manualPoints: false, dimensions: false,
    };
    const blocked = evaluatePostScanAdmission({
      generation: 1,
      snapshot: largeScan,
      profile,
      persistenceBusy: true,
      materialization,
      meshRequested: true,
      aiRequested: false,
    });
    let factoryCalls = 0;
    let materializationCalls = 0;
    const worker = new FakeWorker();
    const client = new SparseTsdfMeshClient(() => {
      factoryCalls += 1;
      return worker as unknown as Worker;
    }, preflightFactory);
    const deferred = runAdmittedTsdfRequest(
      true,
      blocked.admitted.tsdfReconstruction,
      () => {
        materializationCalls += 1;
        return buildTsdfRequestArgs(largeScan);
      },
      (args) => client.reconstruct(args, { postScanWorkerBudget: 1 }),
    );
    expect(deferred).toBeNull();
    expect(materializationCalls).toBe(0);
    expect(factoryCalls).toBe(0);

    const small = postScanSnapshot(gridInput());
    const overlap = evaluatePostScanAdmission({
      generation: 2,
      snapshot: small,
      profile,
      persistenceBusy: true,
      materialization,
      meshRequested: true,
      aiRequested: false,
    });
    expect(overlap.admitted.tsdfReconstruction).toBe(true);
    const pending = runAdmittedTsdfRequest(
      true,
      overlap.admitted.tsdfReconstruction,
      () => {
        materializationCalls += 1;
        return buildTsdfRequestArgs(small);
      },
      (args) => client.reconstruct(args, { postScanWorkerBudget: 1 }),
    );
    expect(pending).not.toBeNull();
    await pending;
    expect(materializationCalls).toBe(1);
    expect(factoryCalls).toBe(1);
    client.dispose();
  });

  it('terminates a pending TSDF worker when admission is revoked', async () => {
    const worker = new HoldingWorker();
    const client = new SparseTsdfMeshClient(
      () => worker as unknown as Worker,
      preflightFactory,
    );
    const pending = client.reconstruct(gridInput());
    await worker.reconstructPosted;

    client.cancel('mesh turned off');

    await expect(pending).rejects.toThrow('mesh turned off');
    expect(worker.terminated).toBe(true);
    expect(worker.messages.some((message) => message.type === 'reset')).toBe(true);
  });

  it('does not create a TSDF worker when physical mesh preflight is not ready', async () => {
    let factoryCalls = 0;
    const client = new SparseTsdfMeshClient(() => {
      factoryCalls += 1;
      return new FakeWorker() as unknown as Worker;
    }, preflightFactory);
    const mesh = await client.reconstruct(gridInput(8, 8, 0.006));
    expect(mesh.triangleCount).toBe(0);
    expect(mesh.meshReadiness).toBeLessThan(1);
    expect(mesh.rawTriangleCount).toBe(0);
    expect(mesh.quarantinedTriangleCount).toBe(0);
    expect(mesh.topologyDiscardedTriangleCount).toBe(0);
    expect(mesh.nonManifoldEdgeCount).toBe(0);
    expect(mesh.nonManifoldRepairTriangleCount).toBe(0);
    expect(mesh.orientationRepairCount).toBe(0);
    expect(mesh.orientationConflictCount).toBe(0);
    expect(mesh.meshTopologyComponentCount).toBe(0);
    expect(mesh.selfIntersectionPairCount).toBe(0);
    expect(mesh.selfIntersectionAffectedTriangleCount).toBe(0);
    expect(mesh.selfIntersectionExactTestedPairs).toBe(0);
    expect(mesh.selfIntersectionBroadPhaseCandidatePairs).toBe(0);
    expect(mesh.selfIntersectionTruncated).toBe(false);
    expect(mesh.selfIntersectionOversizedTriangleCount).toBe(0);
    expect(mesh.localNoiseHotspotCount).toBe(0);
    expect(mesh.localNoiseHotspotTriangleCount).toBe(0);
    expect(mesh.meshNoiseScore).toBe(0);
    expect(factoryCalls).toBe(0);
    client.dispose();
  });

  it('synchronizes epoch when reset happens before the lazy TSDF worker is created', async () => {
    const worker = new FakeWorker();
    const client = new SparseTsdfMeshClient(
      () => worker as unknown as Worker,
      preflightFactory,
    );
    client.reset();
    const mesh = await client.reconstruct(gridInput());

    expect(mesh.meshReadiness).toBe(1);
    // Explicit reset plus request-start cancellation each advance the epoch.
    expect(worker.messages[0]).toMatchObject({ type: 'reset', epoch: 2 });
    expect(worker.messages[1]).toMatchObject({ type: 'reconstruct' });
    client.dispose();
  });
});
