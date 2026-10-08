/** Occupancy snapshot scale fields used to keep FREE/OCCUPIED and UNKNOWN grids distinct. */
export interface OccupancyScaleSnapshot {
  occupancyVoxelSizeMeters: number;
  occupancyFineVoxelSizeMeters?: number | null;
}

/** FREE/OCCUPIED centers use the fine grid; legacy snapshots fall back to UNKNOWN spacing. */
export function fineOccupancyVoxelSizeMeters(snapshot: OccupancyScaleSnapshot): number {
  return snapshot.occupancyFineVoxelSizeMeters ?? snapshot.occupancyVoxelSizeMeters;
}

/** UNKNOWN candidate centers use the effective (possibly coarsened) display grid. */
export function unknownOccupancyVoxelSizeMeters(snapshot: OccupancyScaleSnapshot): number {
  return snapshot.occupancyVoxelSizeMeters;
}
