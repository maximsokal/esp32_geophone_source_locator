export type CandidateVolumePoint = readonly [number, number, number];

export interface CandidateVolumeBounds {
  min: CandidateVolumePoint;
  max: CandidateVolumePoint;
}

export interface CandidateVolumeSnapshotLike {
  bounds?: CandidateVolumeBounds | null;
  occupancyEnvelopeBounds?: CandidateVolumeBounds | null;
  occupancySolverCandidateBelowSurfaceMeters?: number | null;
  occupancyFineVoxelSizeMeters?: number | null;
  occupancyVoxelSizeMeters?: number | null;
}

export interface SolverCandidateVolume {
  min: CandidateVolumePoint | null;
  max: CandidateVolumePoint | null;
  voxelSize: number;
  candidateDepthUnavailable: boolean;
}

function validBounds(bounds: CandidateVolumeBounds | null | undefined): bounds is CandidateVolumeBounds {
  return bounds !== null && bounds !== undefined &&
    bounds.min.length === 3 && bounds.max.length === 3 &&
    bounds.min.every(Number.isFinite) && bounds.max.every(Number.isFinite) &&
    bounds.min.every((value, axis) => value <= bounds.max[axis]);
}

/**
 * The solver's candidate space uses the component-aware occupancy envelope,
 * extended below its lowest surface. It is search support, never occupied
 * evidence. Old scan artifacts fall back to their surface bounds.
 */
export function solverCandidateVolume(snapshotLike: CandidateVolumeSnapshotLike): SolverCandidateVolume {
  const voxelSize = snapshotLike.occupancyFineVoxelSizeMeters ?? snapshotLike.occupancyVoxelSizeMeters ?? Number.NaN;
  const envelope = snapshotLike.occupancyEnvelopeBounds;
  const below = snapshotLike.occupancySolverCandidateBelowSurfaceMeters;
  const candidateMinZ = validBounds(envelope) && below !== undefined && below !== null
    ? envelope.min[2] - below
    : Number.NaN;
  if (validBounds(envelope) && below !== undefined && below !== null && Number.isFinite(below) && below >= 0 &&
      Number.isFinite(candidateMinZ)) {
    return {
      min: [envelope.min[0], envelope.min[1], candidateMinZ],
      max: [envelope.max[0], envelope.max[1], envelope.max[2]],
      voxelSize,
      candidateDepthUnavailable: false,
    };
  }

  const surface = snapshotLike.bounds;
  return {
    min: validBounds(surface) ? [...surface.min] as CandidateVolumePoint : null,
    max: validBounds(surface) ? [...surface.max] as CandidateVolumePoint : null,
    voxelSize,
    candidateDepthUnavailable: true,
  };
}
