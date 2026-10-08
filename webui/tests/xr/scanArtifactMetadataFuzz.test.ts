import { describe, expect, it } from 'vitest';

import { decodeScanArtifact, encodeScanArtifact } from '../../src/xr/scanArtifact';
import type { ScanPointCloudSnapshot } from '../../src/xr/webxrRuntime';
import { rewriteNormalConfidenceAsLegacy, rewriteScanArtifactMetadata } from './support/scanArtifactTestTools';
import { makeValidScanSnapshot } from './support/validScanSnapshot';

interface MutationCase {
  label: string;
  field: string;
  mutate(metadata: Record<string, unknown>): void;
}

function artifactMetadata(bytes: Uint8Array): Record<string, unknown> {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const headerLength = view.getUint32(8, true);
  const header = JSON.parse(new TextDecoder().decode(bytes.subarray(20, 20 + headerLength))) as {
    metadata: Record<string, unknown>;
  };
  return header.metadata;
}

function appDiagnosticProbe(snapshot: ScanPointCloudSnapshot): unknown {
  return {
    manualControlPoints: snapshot.manualControlPoints.map(({ id, position }) => [id, position[0], position[1], position[2]]),
    operatorEdges: snapshot.operatorEdges.map(({ id, start, end, confidence }) => [
      id, start[0], start[1], start[2], end[0], end[1], end[2], confidence,
    ]),
    bounds: snapshot.bounds === null ? null : {
      min: [snapshot.bounds.min[0], snapshot.bounds.min[1], snapshot.bounds.min[2]],
      max: [snapshot.bounds.max[0], snapshot.bounds.max[1], snapshot.bounds.max[2]],
    },
    counters: [
      snapshot.pointCount,
      snapshot.tentativePointCount,
      snapshot.rawPointCount,
      snapshot.depthFrameCount,
      snapshot.sampledDepthFrames,
      snapshot.knownFreeVoxelCount,
      snapshot.occupiedSurfaceVoxelCount,
      snapshot.unknownCandidateVoxelCount,
      snapshot.tsdfPointCount,
    ],
    arrayLengths: [snapshot.positions.length, snapshot.normals.length, snapshot.confidence.length],
  };
}

function seededShuffle<T>(values: T[], initialSeed: number): T[] {
  let state = initialSeed >>> 0;
  const random = (): number => {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    return state / 0x1_0000_0000;
  };
  for (let index = values.length - 1; index > 0; index -= 1) {
    const swap = Math.floor(random() * (index + 1));
    [values[index], values[swap]] = [values[swap], values[index]];
  }
  return values;
}

function decodeOutcome(bytes: Uint8Array): { ok: true; snapshot: ScanPointCloudSnapshot } | { ok: false; error: string } {
  try {
    return { ok: true, snapshot: decodeScanArtifact(bytes) };
  } catch (reason) {
    return { ok: false, error: reason instanceof Error ? `${reason.name}: ${reason.message}` : String(reason) };
  }
}

describe('scan artifact metadata fuzz contract', () => {
  it('rejects malformed metadata deterministically or returns a safe normalized consumer snapshot', () => {
    const base = rewriteNormalConfidenceAsLegacy(encodeScanArtifact(makeValidScanSnapshot()), true);
    const metadata = artifactMetadata(base);
    const mutations: MutationCase[] = [];
    const positiveFields = new Set([
      'voxelSizeMeters', 'maxDepthMeters', 'maxPointCapacity', 'coverageCellSizeMeters',
      'adaptivePointsPerCell', 'memoryBudgetPoints', 'occupancyVoxelSizeMeters', 'occupancyFineVoxelSizeMeters',
    ]);

    for (const [field, value] of Object.entries(metadata)) {
      mutations.push({ label: 'delete', field, mutate: (candidate) => { delete candidate[field]; } });
      mutations.push({ label: 'wrong-type', field, mutate: (candidate) => { candidate[field] = 'invalid'; } });
      mutations.push({ label: 'null', field, mutate: (candidate) => { candidate[field] = null; } });
      if (typeof value === 'number') {
        mutations.push({ label: 'NaN-equivalent', field, mutate: (candidate) => { candidate[field] = Number.NaN; } });
      }
      if (positiveFields.has(field)) {
        mutations.push({ label: 'negative-size', field, mutate: (candidate) => { candidate[field] = -1; } });
        mutations.push({ label: 'zero-size', field, mutate: (candidate) => { candidate[field] = 0; } });
      }
      if (typeof value === 'number' && Number.isSafeInteger(value) && value >= 0) {
        mutations.push({ label: 'huge-count', field, mutate: (candidate) => { candidate[field] = Number.MAX_SAFE_INTEGER + 1; } });
      }
    }
    mutations.push({
      label: 'unknown-additive',
      field: 'futureDiagnostic',
      mutate: (candidate) => { candidate.futureDiagnostic = { revision: 392 }; },
    });

    const orderedMutations = seededShuffle(mutations, 0x392_2026);
    const startedAt = performance.now();
    for (const mutation of orderedMutations) {
      const encoded = rewriteScanArtifactMetadata(base, mutation.mutate);
      const first = decodeOutcome(encoded);
      const second = decodeOutcome(encoded);

      expect(first, `${mutation.label}:${mutation.field}`).toEqual(second);
      if (!first.ok) continue;

      expect(first.snapshot.positions).toBeInstanceOf(Float32Array);
      expect(first.snapshot.normals).toBeInstanceOf(Float32Array);
      expect(first.snapshot.confidence).toBeInstanceOf(Float32Array);
      expect(Array.isArray(first.snapshot.manualControlPoints)).toBe(true);
      expect(Array.isArray(first.snapshot.operatorEdges)).toBe(true);
      expect(() => appDiagnosticProbe(first.snapshot), `${mutation.label}:${mutation.field}`).not.toThrow();
      if (mutation.label === 'unknown-additive') {
        expect(Object.hasOwn(first.snapshot, mutation.field)).toBe(false);
      }
    }
    expect(orderedMutations.length).toBeLessThan(400);
    expect(performance.now() - startedAt).toBeLessThan(2000);
  });
});
