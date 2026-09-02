import { describe, expect, it } from 'vitest';

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

  terminate(): void {}
}

describe('SparseTsdfMeshClient lazy worker lifecycle', () => {
  it('does not create a worker when physical mesh preflight is not ready', async () => {
    let factoryCalls = 0;
    const client = new SparseTsdfMeshClient(() => {
      factoryCalls += 1;
      return new FakeWorker() as unknown as Worker;
    });
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

  it('synchronizes epoch when reset happens before the lazy worker is created', async () => {
    const worker = new FakeWorker();
    const client = new SparseTsdfMeshClient(() => worker as unknown as Worker);
    client.reset();
    const mesh = await client.reconstruct(gridInput());

    expect(mesh.meshReadiness).toBe(1);
    expect(worker.messages[0]).toMatchObject({ type: 'reset', epoch: 1 });
    expect(worker.messages[1]).toMatchObject({ type: 'reconstruct' });
    client.dispose();
  });
});
