import { useEffect, useState } from 'react';

import { loadSessionOccupancyArtifact, loadSessionPreviewArtifact } from '../api/deviceApi';
import { clientDiagnostics } from '../data/clientDiagnostics';
import type { Esp32VoxelView } from '../components/Esp32VoxelLayers';
import { decodeOccupancySolverArtifact } from './occupancySolverArtifact';
import { decodeVoxelPreviewArtifact } from './voxelPreview';

/**
 * The ESP32 voxels of a scan that this phone did not just save (a restored session, the embedded workflow viewer): read back from the device and
 * decoded with the production decoders (issue #44). What is shown is what the device holds, so the status is `saved`; a derivative that belongs to
 * another scan is reported as stale and never drawn.
 */
export type SavedVoxelKind = 'preview' | 'solver';

export type SavedVoxelResult =
  | { state: 'ready'; view: Esp32VoxelView }
  /** The device holds no such derivative for this session. */
  | { state: 'missing' }
  /** It belongs to a different scan (CRC32, frame id or SHA-256 differs). */
  | { state: 'stale' }
  /** The bytes are not a valid artifact. */
  | { state: 'invalid'; error: string };

export interface SavedVoxelRequest {
  objectId: string;
  /** The session id as the store knows it: a recording's, or "staging". */
  sessionId: string;
  kind: SavedVoxelKind;
  /** The identity of the scan on screen, "scan-crc32:xxxxxxxx". */
  scanId: string;
  /** SHA-256 of that scan when known: a derivative that carries one must match it. */
  scanSha256?: string;
}

export interface SavedVoxelLoaders {
  preview(objectId: string, sessionId: string): Promise<Uint8Array | null>;
  occupancy(objectId: string, sessionId: string): Promise<Uint8Array | null>;
}

const DEVICE_LOADERS: SavedVoxelLoaders = { preview: loadSessionPreviewArtifact, occupancy: loadSessionOccupancyArtifact };

function crcOfScanId(scanId: string): number | null {
  const match = /^scan-crc32:([0-9a-f]{8})$/.exec(scanId);
  return match === null ? null : Number.parseInt(match[1], 16) >>> 0;
}

function errorText(reason: unknown): string {
  return reason instanceof Error ? reason.message : String(reason);
}

export async function loadSavedEsp32Voxels(request: SavedVoxelRequest, loaders: SavedVoxelLoaders = DEVICE_LOADERS): Promise<SavedVoxelResult> {
  const bytes = request.kind === 'preview'
    ? await loaders.preview(request.objectId, request.sessionId)
    : await loaders.occupancy(request.objectId, request.sessionId);
  /* One bounded record per load (TASK 11); a rejected identity also says WHICH check failed and what was expected, so a mismatch is never a mystery. */
  const done = (result: SavedVoxelResult): SavedVoxelResult => {
    clientDiagnostics.info('SCAN_VOXEL_LOAD_RESULT', { objectId: request.objectId, sessionId: request.sessionId, kind: request.kind, scan: request.scanId, state: result.state, bytes: bytes?.byteLength ?? 0, ...(result.state === 'invalid' ? { error: result.error.slice(0, 120) } : {}) });
    return result;
  };
  const stale = (mismatch: 'crc32' | 'frame_id' | 'sha256' | 'unparsable_scan_id', expected: string | null, found: string | null): SavedVoxelResult => {
    clientDiagnostics.warn('SCAN_VOXEL_IDENTITY_MISMATCH', { objectId: request.objectId, sessionId: request.sessionId, kind: request.kind, mismatch, expected, found });
    return done({ state: 'stale' });
  };
  if (bytes === null) return done({ state: 'missing' });
  try {
    if (request.kind === 'preview') {
      const artifact = decodeVoxelPreviewArtifact(bytes);
      const crc = crcOfScanId(request.scanId);
      if (crc === null) return stale('unparsable_scan_id', request.scanId, null);
      if (artifact.sourceScanCrc32 >>> 0 !== crc) return stale('crc32', crc.toString(16).padStart(8, '0'), (artifact.sourceScanCrc32 >>> 0).toString(16).padStart(8, '0'));
      return done({ state: 'ready', view: { kind: 'preview', artifact, status: 'saved' } });
    }
    const occupancy = decodeOccupancySolverArtifact(bytes);
    if (occupancy.header.frame_id !== request.scanId) return stale('frame_id', request.scanId, occupancy.header.frame_id);
    const boundSha = occupancy.header.scan_sha256;
    if (boundSha !== undefined && boundSha !== null && request.scanSha256 !== undefined && boundSha !== request.scanSha256) return stale('sha256', request.scanSha256.slice(0, 16), boundSha.slice(0, 16));
    return done({ state: 'ready', view: { kind: 'solver', occupancy, status: 'saved' } });
  } catch (reason) {
    return done({ state: 'invalid', error: errorText(reason) });
  }
}

export type SavedVoxelPhase = 'idle' | 'loading' | 'ready' | 'missing' | 'stale' | 'invalid' | 'failed';

export interface SavedVoxelState {
  phase: SavedVoxelPhase;
  view: Esp32VoxelView | null;
  error: string | null;
}

const IDLE: SavedVoxelState = { phase: 'idle', view: null, error: null };

/**
 * Loads the voxels of one kind for the scan on screen while `request` is non-null. A new request, a null request and an unmount all drop the
 * previous result at once, and a late answer of an older request is ignored, so voxels of another scan never flash into the viewer.
 */
export function useSavedEsp32Voxels(request: SavedVoxelRequest | null, loaders: SavedVoxelLoaders = DEVICE_LOADERS): SavedVoxelState {
  const [state, setState] = useState<SavedVoxelState>(IDLE);
  const key = request === null
    ? null
    : `${request.objectId}/${request.sessionId}/${request.kind}/${request.scanId}/${request.scanSha256 ?? ''}`;
  useEffect(() => {
    if (request === null) { setState(IDLE); return undefined; }
    let cancelled = false;
    setState({ phase: 'loading', view: null, error: null });
    loadSavedEsp32Voxels(request, loaders).then((result) => {
      if (cancelled) return;
      setState(result.state === 'ready'
        ? { phase: 'ready', view: result.view, error: null }
        : { phase: result.state, view: null, error: result.state === 'invalid' ? result.error : null });
    }, (reason: unknown) => {
      if (!cancelled) setState({ phase: 'failed', view: null, error: errorText(reason) });
    });
    return () => { cancelled = true; };
    // `request` is identified by `key`; loaders are stable by contract.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [key]);
  return state;
}
