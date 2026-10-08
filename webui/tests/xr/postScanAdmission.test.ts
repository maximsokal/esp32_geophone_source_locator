import { describe, expect, it } from 'vitest';

import { deriveDeviceComputeProfile } from '../../src/xr/deviceComputeProfile';
import {
  PostScanAdmissionCoordinator,
  evaluatePostScanAdmission,
  type PostScanAdmissionInput,
} from '../../src/xr/postScanAdmission';
import type { ScanPointCloudSnapshot } from '../../src/xr/webxrRuntime';

function profile(deviceMemoryGiB: number | null) {
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

function snapshot(pointCount: number, freePointCount = pointCount): ScanPointCloudSnapshot {
  return {
    positions: new Float32Array(pointCount * 3),
    confidence: new Float32Array(pointCount),
    normals: new Float32Array(pointCount * 3),
    tentativePositions: new Float32Array(0),
    tentativeConfidence: new Float32Array(0),
    freeAirPositions: new Float32Array(freePointCount * 3),
    occupiedSurfacePositions: new Float32Array(0),
    unknownCandidatePositions: new Float32Array(0),
    tsdfPositions: new Float32Array(pointCount * 3),
    tsdfNormals: new Float32Array(pointCount * 3),
    tsdfCoverageConfidence: new Float32Array(pointCount),
    operatorEdges: [],
    manualControlPoints: [],
    bounds: null,
    tsdfPointCount: pointCount,
  } as unknown as ScanPointCloudSnapshot;
}

function typedArrayBytes(snapshot: ScanPointCloudSnapshot): Map<string, Uint8Array> {
  return new Map(Object.entries(snapshot).flatMap(([key, value]) => {
    if (!ArrayBuffer.isView(value)) return [];
    const view = value as ArrayBufferView;
    return [[key, new Uint8Array(view.buffer, view.byteOffset, view.byteLength).slice()] as const];
  }));
}

function request(overrides: Partial<PostScanAdmissionInput> = {}): PostScanAdmissionInput {
  return {
    generation: 1,
    snapshot: snapshot(1000),
    profile: profile(1),
    persistenceBusy: false,
    materialization: {
      good: true,
      bad: false,
      freeAir: false,
      unknownSpace: false,
      manualPoints: false,
      dimensions: false,
    },
    meshRequested: true,
    aiRequested: false,
    ...overrides,
  };
}

describe('post-scan work admission', () => {
  it('holds a large TSDF envelope while persistence is open, but permits small and high-memory overlap', () => {
    const large = snapshot(500_000);
    const constrained = evaluatePostScanAdmission(request({
      snapshot: large,
      persistenceBusy: true,
      aiRequested: true,
    }));
    expect(constrained.stages['primary-durable-persistence']).toBe('active');
    expect(constrained.stages['lightweight-result-shell']).toBe('ready');
    expect(constrained.admitted.viewportPointMaterialization).toBe(false);
    expect(constrained.admitted.tsdfReconstruction).toBe(false);
    expect(constrained.admitted.optionalAi).toBe(false);
    expect(constrained.deferredReason).toBe('primary-persistence-byte-budget');

    const smallOverlap = evaluatePostScanAdmission(request({ persistenceBusy: true }));
    expect(smallOverlap.bytes.safeToOverlapPersistence).toBe(true);
    expect(smallOverlap.admitted.tsdfReconstruction).toBe(true);

    const highMemoryOverlap = evaluatePostScanAdmission(request({
      snapshot: large,
      profile: profile(8),
      persistenceBusy: true,
    }));
    expect(highMemoryOverlap.bytes.safeToOverlapPersistence).toBe(true);
    expect(highMemoryOverlap.admitted.tsdfReconstruction).toBe(true);
  });

  it('reduces post-scan worker capacity for a larger byte envelope at the same CPU count', () => {
    const device = profile(1);
    const small = evaluatePostScanAdmission(request({ profile: device, snapshot: snapshot(1000) }));
    const large = evaluatePostScanAdmission(request({ profile: device, snapshot: snapshot(500_000) }));
    expect(small.bytes.workerConcurrency).toBe(3);
    expect(large.bytes.workerConcurrency).toBe(1);
    expect(large.bytes.totalPlannedBytes).toBeGreaterThan(large.bytes.memoryBudgetBytes);
    expect(large.bytes.underMemoryPressure).toBe(true);
  });

  it('does not plan TSDF slices or workers when mesh is not requested', () => {
    const result = evaluatePostScanAdmission(request({ meshRequested: false }));
    expect(result.admitted.tsdfReconstruction).toBe(false);
    expect(result.bytes.plannedTsdfSliceBytes).toBe(0);
    expect(result.bytes.workerConcurrency).toBe(0);
    expect(result.stages['tsdf-webgpu-reconstruction']).toBe('idle');
  });

  it('defers optional AI before a requested TSDF mesh when its model reservation exceeds memory', () => {
    const constrained = evaluatePostScanAdmission(request({ aiRequested: true }));
    expect(constrained.admitted.tsdfReconstruction).toBe(true);
    expect(constrained.admitted.optionalAi).toBe(false);
    expect(constrained.bytes.optionalAiFitsMemory).toBe(false);
    expect(constrained.stages['optional-ai']).toBe('deferred');

    const highMemory = evaluatePostScanAdmission(request({
      aiRequested: true,
      profile: profile(8),
    }));
    expect(highMemory.admitted.optionalAi).toBe(true);
  });

  it('leaves scan and occupancy bytes unchanged when persistence defers post-scan work', () => {
    const scan = snapshot(1_000, 800);
    scan.positions[0] = 0.375;
    scan.freeAirPositions[2] = -0.625;
    const bytesBefore = typedArrayBytes(scan);
    const state = evaluatePostScanAdmission(request({
      snapshot: scan,
      persistenceBusy: true,
      profile: { ...profile(0.25), postScanMemoryBudgetBytes: 1 },
    }));

    expect(state.admitted.tsdfReconstruction).toBe(false);
    for (const [key, bytes] of bytesBefore) {
      const view = (scan as unknown as Record<string, unknown>)[key] as ArrayBufferView;
      expect(new Uint8Array(view.buffer, view.byteOffset, view.byteLength)).toEqual(bytes);
    }
  });

  it('records each decision with an injectable clock and exposes the current state', () => {
    const decisions: number[] = [];
    const coordinator = new PostScanAdmissionCoordinator(() => 321, (state) => decisions.push(state.evaluatedAtMs));
    const state = coordinator.evaluate(request());
    expect(state.evaluatedAtMs).toBe(321);
    expect(coordinator.getState()).toBe(state);
    expect(decisions).toEqual([321]);
  });
});
