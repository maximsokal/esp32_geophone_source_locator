import type { PreparedDerivatives, ScanPersistenceState } from './scanPersistence';
import { decodeVoxelPreviewArtifact, type VoxelPreviewArtifact } from './voxelPreview';
import { decodeOccupancySolverArtifact, type DecodedOccupancyArtifact } from './occupancySolverArtifact';

/** What the operator is told about the voxels on screen. Never says more than has been verified (issue #44). */
export type Esp32VoxelStatusKey = 'prepared' | 'uploading' | 'saved' | 'failed' | 'stale' | 'unavailable';

export interface Esp32VoxelPreviewState {
  artifact: VoxelPreviewArtifact | null;
  status: Esp32VoxelStatusKey;
  /** Why the bytes could not be shown (a corrupt artifact), else null. */
  error: string | null;
}

export interface Esp32SolverState {
  occupancy: DecodedOccupancyArtifact | null;
  status: Esp32VoxelStatusKey;
  error: string | null;
}

type DerivativeSaveState = ScanPersistenceState['preview'];

/** The device state of one derivative of THIS scan; a persistence state of another scan says nothing about these bytes. */
function deviceStatus(scanId: string, persistence: ScanPersistenceState | null, derivative: DerivativeSaveState | undefined): Esp32VoxelStatusKey {
  if (persistence === null || persistence.scanId !== scanId || (persistence.scan !== 'saved' && persistence.scan !== 'derived_stale') || derivative === undefined) return 'prepared';
  switch (derivative) {
    case 'saving': return 'uploading';
    case 'ready': return 'saved';
    case 'failed': return 'failed';
    case 'stale': return 'stale';
    case 'unavailable': return 'unavailable';
    case 'pending': return 'prepared';
  }
}

/**
 * The state of the TFT preview for the scan being saved: the exact bytes the controller prepared, decoded with the production decoder, and an
 * honest transmission status:
 *  - `prepared`   : the bytes exist and nothing has been confirmed (the scan or the preview upload has not started, or the scan is still being saved);
 *  - `uploading`  : the preview upload is in flight;
 *  - `saved`      : the device accepted the preview for this scan;
 *  - `failed` / `stale` / `unavailable` : as the persistence state says for THIS scan.
 * A persistence state that belongs to another scan says nothing about these bytes, so they stay `prepared`.
 */
export function esp32VoxelPreviewState(prepared: PreparedDerivatives | null, persistence: ScanPersistenceState | null): Esp32VoxelPreviewState | null {
  if (prepared === null || prepared.preview === null) return null;
  let artifact: VoxelPreviewArtifact | null = null;
  let error: string | null = null;
  try {
    artifact = decodeVoxelPreviewArtifact(prepared.preview);
  } catch (reason) {
    error = reason instanceof Error ? reason.message : String(reason);
  }
  const status = deviceStatus(prepared.scanId, persistence, persistence?.preview);
  return { artifact, status, error };
}

/** The same for the solver occupancy (GOCCUPY1): the exact bytes the localisation will receive, decoded, with the device state of THIS scan's occupancy. */
export function esp32SolverState(prepared: PreparedDerivatives | null, persistence: ScanPersistenceState | null): Esp32SolverState | null {
  if (prepared === null || prepared.occupancy === null) return null;
  let occupancy: DecodedOccupancyArtifact | null = null;
  let error: string | null = null;
  try {
    occupancy = decodeOccupancySolverArtifact(prepared.occupancy);
  } catch (reason) {
    error = reason instanceof Error ? reason.message : String(reason);
  }
  return { occupancy, status: deviceStatus(prepared.scanId, persistence, persistence?.occupancy), error };
}
