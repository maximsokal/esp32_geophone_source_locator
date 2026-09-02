import { VerifiedFreeSpaceIndex, type VerifiedFreeSpaceSnapshot } from './verifiedFreeSpace';

export interface PositionedStructuralCandidate {
  position: readonly [number, number, number];
}

export interface StructuralFreeSpaceGateResult<T> {
  candidates: T[];
  rejectedByFreeSpace: number;
}

/**
 * Mandatory final safety gate for inferred structural point candidates.
 *
 * A detector may use different geometric evidence internally, but no inferred
 * corner/endpoint is allowed to survive if its final position lies in a voxel
 * already verified FREE by physical ray carving. Exact-cell semantics preserve
 * UNKNOWN around the voxel and therefore cannot grow free space implicitly.
 */
export function rejectStructuralCandidatesInVerifiedFreeSpace<T extends PositionedStructuralCandidate>(
  candidates: readonly T[],
  snapshot: VerifiedFreeSpaceSnapshot,
): StructuralFreeSpaceGateResult<T> {
  const freeSpace = VerifiedFreeSpaceIndex.from(snapshot);
  if (freeSpace === null || candidates.length === 0) {
    return { candidates: [...candidates], rejectedByFreeSpace: 0 };
  }

  const accepted: T[] = [];
  let rejectedByFreeSpace = 0;
  for (const candidate of candidates) {
    if (freeSpace.contains(candidate.position)) {
      rejectedByFreeSpace += 1;
      continue;
    }
    accepted.push(candidate);
  }
  return { candidates: accepted, rejectedByFreeSpace };
}
