import { BulkTransferPreemptedError, type ControlResult, type ScanArtifactInfo } from '../api/deviceApi';
import { occupancyFrameIdForScan } from './occupancySolverArtifact';

export type ScanSaveState = 'saving' | 'saved' | 'error' | 'blocked_recording';
export type OccupancySaveState = 'pending' | 'saving' | 'ready' | 'failed' | 'unavailable';

export const SCAN_STORAGE_BLOCKED_MESSAGE = 'Recording in progress — scan storage is paused';
export const SCAN_STORAGE_PREEMPTED_MESSAGE = 'Scan save paused for a critical control action. Retry to save it.';

export interface ScanPersistenceState {
  scan: ScanSaveState;
  occupancy: OccupancySaveState;
  scanError: string | null;
  occupancyError: string | null;
}

export type ScanPersistenceEvent =
  | { type: 'scan-saving' }
  | { type: 'scan-saved' }
  | { type: 'scan-failed'; error: string }
  | { type: 'scan-blocked-recording' }
  | { type: 'scan-preempted' }
  | { type: 'occupancy-saving' }
  | { type: 'occupancy-ready' }
  | { type: 'occupancy-failed'; error: string }
  | { type: 'occupancy-unavailable'; error: string }
  | { type: 'reconcile-ready' }
  | { type: 'reconcile-missing-retryable' }
  | { type: 'reconcile-missing-unavailable'; error: string }
  | { type: 'reconcile-error'; error: string };

function invalidTransition(state: ScanPersistenceState | null, event: ScanPersistenceEvent): never {
  throw new Error(`Invalid scan persistence transition ${state?.scan ?? 'none'}/${state?.occupancy ?? 'none'} -> ${event.type}.`);
}

export function transitionScanPersistence(
  state: ScanPersistenceState | null,
  event: ScanPersistenceEvent,
): ScanPersistenceState {
  if (event.type === 'scan-saving') {
    return { scan: 'saving', occupancy: 'pending', scanError: null, occupancyError: null };
  }
  if (event.type === 'reconcile-ready') {
    return { scan: 'saved', occupancy: 'ready', scanError: null, occupancyError: null };
  }
  if (event.type === 'reconcile-missing-retryable') {
    return {
      scan: 'saved', occupancy: 'failed', scanError: null,
      occupancyError: 'Saved scan is present; occupancy is missing. Retry will reuse the matching in-memory bytes.',
    };
  }
  if (event.type === 'reconcile-missing-unavailable') {
    return { scan: 'saved', occupancy: 'unavailable', scanError: null, occupancyError: event.error };
  }
  if (event.type === 'reconcile-error') {
    return { scan: 'saved', occupancy: 'unavailable', scanError: null, occupancyError: event.error };
  }

  if (event.type === 'scan-saved' && state?.scan === 'saving') {
    return { scan: 'saved', occupancy: 'pending', scanError: null, occupancyError: null };
  }
  if (event.type === 'scan-failed' && state?.scan === 'saving') {
    return { scan: 'error', occupancy: 'unavailable', scanError: event.error, occupancyError: null };
  }
  if (event.type === 'scan-blocked-recording' && state?.scan === 'saving') {
    return { scan: 'blocked_recording', occupancy: 'pending', scanError: SCAN_STORAGE_BLOCKED_MESSAGE, occupancyError: null };
  }
  if (event.type === 'scan-preempted' && state?.scan === 'saving') {
    return { scan: 'blocked_recording', occupancy: 'pending', scanError: SCAN_STORAGE_PREEMPTED_MESSAGE, occupancyError: null };
  }
  if (event.type === 'occupancy-saving' && state?.scan === 'saved' && (state.occupancy === 'pending' || state.occupancy === 'failed')) {
    return { ...state, occupancy: 'saving', occupancyError: null };
  }
  if (event.type === 'occupancy-ready' && state?.scan === 'saved' && state.occupancy === 'saving') {
    return { ...state, occupancy: 'ready', occupancyError: null };
  }
  if (event.type === 'occupancy-failed' && state?.scan === 'saved' && state.occupancy === 'saving') {
    return { ...state, occupancy: 'failed', occupancyError: event.error };
  }
  if (event.type === 'occupancy-unavailable' && state?.scan === 'saved' && state.occupancy === 'pending') {
    return { ...state, occupancy: 'unavailable', occupancyError: event.error };
  }
  return invalidTransition(state, event);
}

export interface ScanPersistencePorts {
  saveScan(objectId: string, sessionId: string, bytes: Uint8Array): Promise<ControlResult>;
  saveOccupancy(objectId: string, sessionId: string, bytes: Uint8Array): Promise<ControlResult>;
  fetchInfo(objectId: string, sessionId: string, kind: 'scan' | 'occupancy'): Promise<ScanArtifactInfo>;
}

interface PendingOccupancy {
  objectId: string;
  sessionId: string;
  scanFrameId: string;
  bytes: Uint8Array;
}

interface PendingScan {
  objectId: string;
  sessionId: string;
  bytes: Uint8Array;
  buildOccupancy: () => Uint8Array;
}

function errorText(reason: unknown): string {
  return reason instanceof Error ? reason.message : String(reason);
}

function requireAccepted(result: ControlResult): void {
  if (result?.ok !== true) throw new Error(result?.error ?? 'ESP scan save was not accepted.');
}

function isBlockedRecording(value: unknown): boolean {
  return typeof value === 'object' && value !== null &&
    'outcome' in value && (value as { outcome?: unknown }).outcome === 'blocked_recording' &&
    'busy_reason' in value && (value as { busy_reason?: unknown }).busy_reason === 'recording_active';
}

/** Keeps one retry payload in memory, keyed to the exact saved session scan CRC. */
export class ScanPersistenceController {
  private state: ScanPersistenceState | null = null;
  private pendingOccupancy: PendingOccupancy | null = null;
  private pendingScan: PendingScan | null = null;
  private epoch = 0;

  constructor(
    private readonly ports: ScanPersistencePorts,
    private readonly onState: (state: ScanPersistenceState | null) => void = () => undefined,
  ) {}

  get currentState(): ScanPersistenceState | null { return this.state; }

  get retryAvailable(): boolean {
    return (this.state?.scan === 'blocked_recording' && this.pendingScan !== null) ||
      (this.state?.scan === 'saved' && this.state.occupancy === 'failed' && this.pendingOccupancy !== null);
  }

  reset(): void {
    this.epoch += 1;
    this.pendingOccupancy = null;
    this.pendingScan = null;
    this.state = null;
    this.onState(null);
  }

  async save(
    objectId: string,
    sessionId: string,
    scanBytes: Uint8Array,
    buildOccupancy: () => Uint8Array,
    blockedByRecording = false,
  ): Promise<ScanPersistenceState | null> {
    const epoch = ++this.epoch;
    this.pendingOccupancy = null;
    const pending: PendingScan = { objectId, sessionId, bytes: scanBytes.slice(), buildOccupancy };
    this.pendingScan = pending;
    this.dispatch(epoch, { type: 'scan-saving' });
    if (blockedByRecording) {
      this.dispatch(epoch, { type: 'scan-blocked-recording' });
      return this.state;
    }
    return this.uploadScan(epoch, pending);
  }

  private async uploadScan(epoch: number, pending: PendingScan): Promise<ScanPersistenceState | null> {
    try {
      const result = await this.ports.saveScan(pending.objectId, pending.sessionId, pending.bytes);
      if (epoch !== this.epoch) return this.state;
      if (isBlockedRecording(result)) {
        this.dispatch(epoch, { type: 'scan-blocked-recording' });
        return this.state;
      }
      requireAccepted(result);
    } catch (reason) {
      if (epoch !== this.epoch) return this.state;
      if (isBlockedRecording(reason)) {
        this.dispatch(epoch, { type: 'scan-blocked-recording' });
        return this.state;
      }
      if (reason instanceof BulkTransferPreemptedError) {
        this.dispatch(epoch, { type: 'scan-preempted' });
        return this.state;
      }
      this.pendingScan = null;
      this.dispatch(epoch, { type: 'scan-failed', error: errorText(reason) });
      return this.state;
    }
    if (epoch !== this.epoch) return this.state;
    this.pendingScan = null;
    this.dispatch(epoch, { type: 'scan-saved' });

    try {
      this.pendingOccupancy = {
        objectId: pending.objectId,
        sessionId: pending.sessionId,
        scanFrameId: occupancyFrameIdForScan(pending.bytes),
        bytes: pending.buildOccupancy().slice(),
      };
    } catch (reason) {
      this.dispatch(epoch, { type: 'occupancy-unavailable', error: `Occupancy could not be prepared: ${errorText(reason)}` });
      return this.state;
    }
    return this.uploadOccupancy(epoch);
  }

  async retry(objectId: string, sessionId: string): Promise<ScanPersistenceState | null> {
    const epoch = ++this.epoch;
    const pendingScan = this.pendingScan;
    if (this.state?.scan === 'blocked_recording' && pendingScan !== null &&
        pendingScan.objectId === objectId && pendingScan.sessionId === sessionId) {
      this.dispatch(epoch, { type: 'scan-saving' });
      return this.uploadScan(epoch, pendingScan);
    }
    const pending = this.pendingOccupancy;
    if (this.state?.scan !== 'saved' || this.state.occupancy !== 'failed' || pending === null ||
        pending.objectId !== objectId || pending.sessionId !== sessionId) {
      if (this.state?.scan === 'saved') {
        this.dispatch(epoch, {
          type: 'reconcile-missing-unavailable',
          error: 'Matching occupancy bytes are not in memory. Rescan and re-save the scan to create them safely.',
        });
      }
      return this.state;
    }
    return this.uploadOccupancy(epoch);
  }

  async reconcile(
    objectId: string,
    sessionId: string,
    savedScanBytes: Uint8Array | null,
  ): Promise<ScanPersistenceState | null> {
    const epoch = ++this.epoch;
    let scanInfo: ScanArtifactInfo;
    let occupancyInfo: ScanArtifactInfo;
    try {
      [scanInfo, occupancyInfo] = await Promise.all([
        this.ports.fetchInfo(objectId, sessionId, 'scan'),
        this.ports.fetchInfo(objectId, sessionId, 'occupancy'),
      ]);
    } catch (reason) {
      if (epoch === this.epoch && savedScanBytes !== null) {
        this.dispatch(epoch, { type: 'reconcile-error', error: `Could not verify occupancy persistence: ${errorText(reason)}` });
      }
      return this.state;
    }
    if (epoch !== this.epoch) return this.state;
    if (!scanInfo.present) {
      this.state = null;
      this.onState(null);
      return null;
    }
    if (occupancyInfo.present) {
      this.dispatch(epoch, { type: 'reconcile-ready' });
      return this.state;
    }
    const pending = this.pendingOccupancy;
    const matchingBytes = savedScanBytes !== null && pending !== null &&
      pending.objectId === objectId && pending.sessionId === sessionId &&
      pending.scanFrameId === occupancyFrameIdForScan(savedScanBytes) &&
      scanInfo.bytes === savedScanBytes.byteLength;
    if (matchingBytes) this.dispatch(epoch, { type: 'reconcile-missing-retryable' });
    else this.dispatch(epoch, {
      type: 'reconcile-missing-unavailable',
      error: 'Occupancy is missing and matching bytes are not in memory. Rescan and re-save the scan to create them safely.',
    });
    return this.state;
  }

  private async uploadOccupancy(epoch: number): Promise<ScanPersistenceState | null> {
    const pending = this.pendingOccupancy;
    if (pending === null) return this.state;
    this.dispatch(epoch, { type: 'occupancy-saving' });
    try {
      requireAccepted(await this.ports.saveOccupancy(pending.objectId, pending.sessionId, pending.bytes));
      this.dispatch(epoch, { type: 'occupancy-ready' });
    } catch (reason) {
      this.dispatch(epoch, { type: 'occupancy-failed', error: errorText(reason) });
    }
    return this.state;
  }

  private dispatch(epoch: number, event: ScanPersistenceEvent): void {
    if (epoch !== this.epoch) return;
    this.state = transitionScanPersistence(this.state, event);
    this.onState(this.state);
  }
}
