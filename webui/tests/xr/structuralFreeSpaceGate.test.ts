import { describe, expect, it } from 'vitest';

import { rejectStructuralCandidatesInVerifiedFreeSpace } from '../../src/xr/structuralFreeSpaceGate';
import { VerifiedFreeSpaceIndex } from '../../src/xr/verifiedFreeSpace';

describe('verified structural free-space gate', () => {
  it('builds no index without valid verified free-space evidence', () => {
    expect(VerifiedFreeSpaceIndex.from({})).toBeNull();
    expect(VerifiedFreeSpaceIndex.from({
      freeSpacePositions: Float32Array.from([0, 0, 0]),
      freeSpaceVoxelSizeMeters: 0,
    })).toBeNull();
  });

  it('uses exact voxel cells and does not dilate FREE into adjacent UNKNOWN', () => {
    const index = VerifiedFreeSpaceIndex.from({
      freeSpacePositions: Float32Array.from([0.011, 0.011, 0.011]),
      freeSpaceVoxelSizeMeters: 0.04,
    });
    expect(index).not.toBeNull();
    expect(index!.contains([0.039, 0.039, 0.039])).toBe(true);
    expect(index!.contains([0.041, 0.039, 0.039])).toBe(false);
  });

  it('handles negative coordinates with floor-based voxel ownership', () => {
    const index = VerifiedFreeSpaceIndex.from({
      freeSpacePositions: Float32Array.from([-0.001, -0.001, -0.001]),
      freeSpaceVoxelSizeMeters: 0.04,
    });
    expect(index).not.toBeNull();
    expect(index!.contains([-0.039, -0.039, -0.039])).toBe(true);
    expect(index!.contains([0.001, -0.039, -0.039])).toBe(false);
  });

  it('rejects only inferred candidates whose final position is verified FREE', () => {
    const candidates = [
      { id: 'free-corner', position: [0.012, 0.012, 0.012] as const, confidence: 0.91 },
      { id: 'occupied-or-unknown-corner', position: [0.052, 0.012, 0.012] as const, confidence: 0.88 },
    ];
    const result = rejectStructuralCandidatesInVerifiedFreeSpace(candidates, {
      freeSpacePositions: Float32Array.from([0.01, 0.01, 0.01]),
      freeSpaceVoxelSizeMeters: 0.04,
    });
    expect(result.rejectedByFreeSpace).toBe(1);
    expect(result.candidates.map((candidate) => candidate.id)).toEqual(['occupied-or-unknown-corner']);
  });

  it('fails open when no verified FREE set exists', () => {
    const candidates = [{ position: [0, 0, 0] as const, confidence: 0.9 }];
    const result = rejectStructuralCandidatesInVerifiedFreeSpace(candidates, {});
    expect(result.rejectedByFreeSpace).toBe(0);
    expect(result.candidates).toHaveLength(1);
  });
});
