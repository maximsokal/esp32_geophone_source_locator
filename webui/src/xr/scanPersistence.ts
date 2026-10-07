import {
  BulkTransferPreemptedError,
  retryPreemptedRead,
  ScanWriteOutcomeError,
  type ControlResult,
  type ScanArtifactInfo,
} from '../api/deviceApi';
import { occupancyFrameIdForScan } from './occupancySolverArtifact';

export type ScanSaveState = 'saving' | 'verifying' | 'saved' | 'error' | 'unknown' | 'derived_stale' | 'blocked_recording';
export type OccupancySaveState = 'pending' | 'saving' | 'ready' | 'failed' | 'stale' | 'unavailable';
export type PreviewSaveState = 'pending' | 'saving' | 'ready' | 'failed' | 'stale' | 'unavailable';
export type ScanPersistenceOutcome = 'confirmed_committed' | 'confirmed_not_committed' | 'unknown';

export const SCAN_STORAGE_BLOCKED_MESSAGE = 'Recording in progress — scan storage is paused';
export const SCAN_STORAGE_PREEMPTED_MESSAGE = 'Scan save paused for a critical control action. Retry to save it.';

export interface ScanPersistenceState {
  scan: ScanSaveState;
  occupancy: OccupancySaveState;
  preview: PreviewSaveState;
  scanError: string | null;
  occupancyError: string | null;
  previewError: string | null;
  scanId: string | null;
  outcome: ScanPersistenceOutcome | null;
  previousKept: boolean;
}

export type ScanPersistenceEvent =
  | { type: 'scan-saving'; scanId: string }
  | { type: 'scan-saved'; scanId: string }
  | { type: 'scan-verifying'; scanId: string }
  | { type: 'scan-not-committed'; scanId: string; error: string; previousKept: boolean; restoredState?: ScanPersistenceState }
  | { type: 'scan-unknown'; scanId: string; error: string }
  | { type: 'scan-derived-stale'; scanId: string; error: string }
  | { type: 'scan-blocked-recording'; scanId: string }
  | { type: 'occupancy-saving' }
  | { type: 'occupancy-ready' }
  | { type: 'occupancy-failed'; error: string }
  | { type: 'occupancy-unavailable'; error: string }
  | { type: 'occupancy-stale'; error: string }
  | { type: 'preview-saving' }
  | { type: 'preview-ready' }
  | { type: 'preview-failed'; error: string }
  | { type: 'preview-unavailable'; error: string }
  | { type: 'preview-stale'; error: string }
  | { type: 'reconcile-saved'; scanId: string }
  | { type: 'reconcile-unknown'; scanId: string | null; error: string }
  | { type: 'reconcile-occupancy-ready' }
  | { type: 'reconcile-occupancy-missing-retryable' }
  | { type: 'reconcile-occupancy-unavailable'; error: string }
  | { type: 'reconcile-occupancy-stale'; error: string }
  | { type: 'reconcile-preview-ready' }
  | { type: 'reconcile-preview-missing-retryable' }
  | { type: 'reconcile-preview-unavailable'; error: string }
  | { type: 'reconcile-preview-stale'; error: string };

function emptyState(scan: ScanSaveState, scanId: string | null = null): ScanPersistenceState {
  return {
    scan,
    occupancy: 'pending',
    preview: 'pending',
    scanError: null,
    occupancyError: null,
    previewError: null,
    scanId,
    outcome: null,
    previousKept: false,
  };
}

function invalidTransition(state: ScanPersistenceState | null, event: ScanPersistenceEvent): never {
  throw new Error(`Invalid scan persistence transition ${state?.scan ?? 'none'}/${state?.occupancy ?? 'none'} -> ${event.type}.`);
}

export function transitionScanPersistence(
  state: ScanPersistenceState | null,
  event: ScanPersistenceEvent,
): ScanPersistenceState {
  switch (event.type) {
    case 'scan-saving':
      return emptyState('saving', event.scanId);
    case 'scan-saved':
      if (state?.scan !== 'saving' && state?.scan !== 'verifying') return invalidTransition(state, event);
      return { ...emptyState('saved', event.scanId), outcome: 'confirmed_committed' };
    case 'scan-verifying':
      if (state?.scan !== 'saving' && state?.scan !== 'verifying' && state?.scan !== 'unknown') return invalidTransition(state, event);
      return { ...emptyState('verifying', event.scanId), outcome: 'unknown' };
    case 'scan-not-committed':
      return event.previousKept && event.restoredState?.scan === 'saved'
        ? { ...event.restoredState, scan: 'error', scanError: event.error, scanId: event.scanId, outcome: 'confirmed_not_committed', previousKept: true }
        : { ...emptyState('error', event.scanId), scanError: event.error, outcome: 'confirmed_not_committed', previousKept: event.previousKept };
    case 'scan-unknown':
      return { ...emptyState('unknown', event.scanId), scanError: event.error, outcome: 'unknown' };
    case 'scan-derived-stale':
      return {
        ...emptyState('derived_stale', event.scanId),
        occupancy: 'stale',
        preview: 'stale',
        scanError: event.error,
        occupancyError: event.error,
        previewError: event.error,
        outcome: 'unknown',
      };
    case 'scan-blocked-recording':
      return { ...emptyState('blocked_recording', event.scanId), scanError: SCAN_STORAGE_BLOCKED_MESSAGE };
    case 'occupancy-saving':
      if (state?.scan !== 'saved' || (state.occupancy !== 'pending' && state.occupancy !== 'failed' && state.occupancy !== 'stale')) return invalidTransition(state, event);
      return { ...state, occupancy: 'saving', occupancyError: null };
    case 'occupancy-ready':
      if (state?.scan !== 'saved' || state.occupancy !== 'saving') return invalidTransition(state, event);
      return { ...state, occupancy: 'ready', occupancyError: null };
    case 'reconcile-occupancy-ready':
      if (state?.scan !== 'saved') return invalidTransition(state, event);
      return { ...state, occupancy: 'ready', occupancyError: null };
    case 'occupancy-failed':
      if (state?.scan !== 'saved' || (state.occupancy !== 'saving' && state.occupancy !== 'pending' && state.occupancy !== 'stale')) return invalidTransition(state, event);
      return { ...state, occupancy: 'failed', occupancyError: event.error };
    case 'occupancy-unavailable':
      if (state?.scan !== 'saved') return invalidTransition(state, event);
      return { ...state, occupancy: 'unavailable', occupancyError: event.error };
    case 'occupancy-stale':
    case 'reconcile-occupancy-stale':
      if (state?.scan !== 'saved') return invalidTransition(state, event);
      return { ...state, occupancy: 'stale', occupancyError: event.error };
    case 'preview-saving':
      if (state?.scan !== 'saved' || (state.preview !== 'pending' && state.preview !== 'failed' && state.preview !== 'stale')) return invalidTransition(state, event);
      return { ...state, preview: 'saving', previewError: null };
    case 'preview-ready':
      if (state?.scan !== 'saved' || state.preview !== 'saving') return invalidTransition(state, event);
      return { ...state, preview: 'ready', previewError: null };
    case 'reconcile-preview-ready':
      if (state?.scan !== 'saved') return invalidTransition(state, event);
      return { ...state, preview: 'ready', previewError: null };
    case 'preview-failed':
      if (state?.scan !== 'saved' || (state.preview !== 'saving' && state.preview !== 'pending' && state.preview !== 'stale')) return invalidTransition(state, event);
      return { ...state, preview: 'failed', previewError: event.error };
    case 'preview-unavailable':
      if (state?.scan !== 'saved') return invalidTransition(state, event);
      return { ...state, preview: 'unavailable', previewError: event.error };
    case 'preview-stale':
    case 'reconcile-preview-stale':
      if (state?.scan !== 'saved') return invalidTransition(state, event);
      return { ...state, preview: 'stale', previewError: event.error };
    case 'reconcile-saved':
      return { ...emptyState('saved', event.scanId), outcome: 'confirmed_committed' };
    case 'reconcile-unknown':
      return { ...emptyState('unknown', event.scanId), scanError: event.error, outcome: 'unknown' };
    case 'reconcile-occupancy-missing-retryable':
      if (state?.scan !== 'saved') return invalidTransition(state, event);
      return { ...state, occupancy: 'failed', occupancyError: 'Saved scan is present; occupancy is missing. Retry will reuse the matching in-memory bytes.' };
    case 'reconcile-occupancy-unavailable':
      if (state?.scan !== 'saved') return invalidTransition(state, event);
      return { ...state, occupancy: 'unavailable', occupancyError: event.error };
    case 'reconcile-preview-missing-retryable':
      if (state?.scan !== 'saved') return invalidTransition(state, event);
      return { ...state, preview: 'failed', previewError: 'Saved scan is present; preview is missing. Retry will reuse the matching in-memory bytes.' };
    case 'reconcile-preview-unavailable':
      if (state?.scan !== 'saved') return invalidTransition(state, event);
      return { ...state, preview: 'unavailable', previewError: event.error };
  }
  return invalidTransition(state, event);
}

export interface ScanPersistencePorts {
  saveScan(objectId: string, sessionId: string, bytes: Uint8Array): Promise<ControlResult>;
  saveOccupancy(objectId: string, sessionId: string, bytes: Uint8Array, scanId?: string): Promise<ControlResult>;
  savePreview(objectId: string, sessionId: string, bytes: Uint8Array, scanId?: string): Promise<ControlResult>;
  fetchInfo(objectId: string, sessionId: string, kind: 'scan' | 'occupancy' | 'preview'): Promise<ScanArtifactInfo>;
}

export function scanPersistenceSessionId(sessionId: string | null): string {
  return sessionId ?? 'staging';
}

interface ScanIdentity {
  known: boolean;
  present: boolean;
  id: string | null;
}

interface PendingOccupancy {
  objectId: string;
  sessionId: string;
  scanId: string;
  bytes: Uint8Array;
}

interface PendingPreview {
  objectId: string;
  sessionId: string;
  scanId: string;
  bytes: Uint8Array;
}

interface PendingScan {
  objectId: string;
  sessionId: string;
  bytes: Uint8Array;
  scanId: string;
  previous: ScanIdentity;
  previousPersistence: ScanPersistenceState | null;
  buildOccupancy: () => Uint8Array;
  buildPreview: (occupancyBytes: Uint8Array | null) => Uint8Array;
}

function errorText(reason: unknown): string {
  return reason instanceof Error ? reason.message : String(reason);
}

function infoIdentity(info: ScanArtifactInfo): ScanIdentity {
  return { known: true, present: info.present, id: info.id ?? null };
}

function sameScan(left: ScanIdentity, right: ScanArtifactInfo): boolean {
  if (!left.known) return false;
  if (!left.present) return !right.present;
  return left.id !== null && right.present && right.id === left.id;
}

function isBlockedRecording(value: unknown): boolean {
  return typeof value === 'object' && value !== null &&
    'outcome' in value && (value as { outcome?: unknown }).outcome === 'blocked_recording' &&
    'busy_reason' in value && (value as { busy_reason?: unknown }).busy_reason === 'recording_active';
}

function isGenerationMismatch(value: unknown): value is ScanWriteOutcomeError {
  return value instanceof ScanWriteOutcomeError && /scan_generation_mismatch/.test(value.message);
}

function generationMismatchResult(value: ControlResult): boolean {
  return value.ok === false && value.error === 'scan_generation_mismatch';
}

function requireAccepted(result: ControlResult): void {
  if (result?.ok !== true) throw new Error(result?.error ?? 'ESP scan save was not accepted.');
}

/** Keeps exact candidate and derivative bytes until their generation is confirmed on the device. */
export class ScanPersistenceController {
  private state: ScanPersistenceState | null = null;
  private pendingOccupancy: PendingOccupancy | null = null;
  private pendingPreview: PendingPreview | null = null;
  private pendingScan: PendingScan | null = null;
  private knownScan: { key: string; identity: ScanIdentity } | null = null;
  private epoch = 0;

  constructor(
    private readonly ports: ScanPersistencePorts,
    private readonly onState: (state: ScanPersistenceState | null) => void = () => undefined,
  ) {}

  get currentState(): ScanPersistenceState | null { return this.state; }

  get retryAvailable(): boolean {
    return (this.state?.scan === 'blocked_recording' && this.pendingScan !== null) ||
      ((this.state?.scan === 'error' || this.state?.scan === 'unknown') && this.pendingScan !== null) ||
      (this.state?.scan === 'saved' && (this.state.occupancy === 'failed' || this.state.occupancy === 'stale') &&
        (this.pendingOccupancy !== null || this.pendingScan !== null)) ||
      (this.state?.scan === 'saved' && (this.state.preview === 'failed' || this.state.preview === 'stale') &&
        (this.pendingPreview !== null || this.pendingScan !== null));
  }

  reset(): void {
    this.epoch += 1;
    this.pendingOccupancy = null;
    this.pendingPreview = null;
    this.pendingScan = null;
    this.knownScan = null;
    this.state = null;
    this.onState(null);
  }

  async save(
    objectId: string,
    sessionId: string,
    scanBytes: Uint8Array,
    buildOccupancy: () => Uint8Array,
    buildPreview: (occupancyBytes: Uint8Array | null) => Uint8Array,
    blockedByRecording = false,
    candidateId?: string,
  ): Promise<ScanPersistenceState | null> {
    const epoch = ++this.epoch;
    this.pendingOccupancy = null;
    this.pendingPreview = null;
    const bytes = scanBytes.slice();
    const key = `${objectId}/${sessionId}`;
    const pending: PendingScan = {
      objectId,
      sessionId,
      bytes,
      scanId: candidateId ?? occupancyFrameIdForScan(bytes),
      previous: this.knownScan?.key === key ? this.knownScan.identity : { known: false, present: false, id: null },
      previousPersistence: this.state,
      buildOccupancy,
      buildPreview,
    };
    this.pendingScan = pending;
    this.dispatch(epoch, { type: 'scan-saving', scanId: pending.scanId });
    if (blockedByRecording) {
      this.dispatch(epoch, { type: 'scan-blocked-recording', scanId: pending.scanId });
      return this.state;
    }
    if (!pending.previous.known) {
      try {
        pending.previous = infoIdentity(await this.readInfo(objectId, sessionId, 'scan'));
        this.knownScan = { key, identity: pending.previous };
      } catch {
        pending.previous = { known: false, present: false, id: null };
      }
      if (epoch !== this.epoch) return this.state;
    }
    return this.uploadScan(epoch, pending);
  }

  private async uploadScan(epoch: number, pending: PendingScan): Promise<ScanPersistenceState | null> {
    try {
      const result = await this.ports.saveScan(pending.objectId, pending.sessionId, pending.bytes);
      if (epoch !== this.epoch) return this.state;
      if (isBlockedRecording(result)) {
        this.dispatch(epoch, { type: 'scan-blocked-recording', scanId: pending.scanId });
        return this.state;
      }
      if (result?.ok !== true) {
        await this.confirmNotCommitted(epoch, pending, result?.error ?? 'ESP scan save was rejected.');
        return this.state;
      }
      if (result.id !== pending.scanId) return this.verifyUnknown(epoch, pending, 'Scan save reply did not match the candidate identity.');
    } catch (reason) {
      if (epoch !== this.epoch) return this.state;
      if (isBlockedRecording(reason)) {
        this.dispatch(epoch, { type: 'scan-blocked-recording', scanId: pending.scanId });
        return this.state;
      }
      if (reason instanceof ScanWriteOutcomeError && reason.outcome === 'confirmed_not_committed') {
        await this.confirmNotCommitted(epoch, pending, errorText(reason));
        return this.state;
      }
      return this.verifyUnknown(epoch, pending, errorText(reason));
    }
    if (epoch !== this.epoch) return this.state;
    return this.commitScan(epoch, pending);
  }

  private async verifyUnknown(epoch: number, pending: PendingScan, error: string): Promise<ScanPersistenceState | null> {
    this.dispatch(epoch, { type: 'scan-verifying', scanId: pending.scanId });
    try {
      const current = await this.readInfo(pending.objectId, pending.sessionId, 'scan');
      if (epoch !== this.epoch) return this.state;
      if (current.present && current.id === pending.scanId) return this.commitScan(epoch, pending);
      if (sameScan(pending.previous, current)) {
        this.knownScan = { key: `${pending.objectId}/${pending.sessionId}`, identity: infoIdentity(current) };
        const previousKept = sameScan(pending.previous, current) && current.present;
        this.dispatch(epoch, {
          type: 'scan-not-committed', scanId: pending.scanId, error, previousKept,
          restoredState: previousKept && pending.previousPersistence?.scan === 'saved' &&
            pending.previousPersistence.scanId === pending.previous.id ? pending.previousPersistence : undefined,
        });
        return this.state;
      }
      this.knownScan = { key: `${pending.objectId}/${pending.sessionId}`, identity: infoIdentity(current) };
    } catch {
      if (epoch !== this.epoch) return this.state;
    }
    this.dispatch(epoch, { type: 'scan-unknown', scanId: pending.scanId, error });
    return this.state;
  }

  private async confirmNotCommitted(epoch: number, pending: PendingScan, error: string): Promise<void> {
    let previousKept = false;
    try {
      const current = await this.readInfo(pending.objectId, pending.sessionId, 'scan');
      if (epoch !== this.epoch) return;
      previousKept = sameScan(pending.previous, current) && current.present;
      this.knownScan = { key: `${pending.objectId}/${pending.sessionId}`, identity: infoIdentity(current) };
    } catch {
      if (epoch !== this.epoch) return;
    }
    this.dispatch(epoch, {
      type: 'scan-not-committed', scanId: pending.scanId, error, previousKept,
      restoredState: previousKept && pending.previousPersistence?.scan === 'saved' &&
        pending.previousPersistence.scanId === pending.previous.id ? pending.previousPersistence : undefined,
    });
  }

  private async commitScan(epoch: number, pending: PendingScan): Promise<ScanPersistenceState | null> {
    if (epoch !== this.epoch) return this.state;
    this.knownScan = { key: `${pending.objectId}/${pending.sessionId}`, identity: { known: true, present: true, id: pending.scanId } };
    this.pendingOccupancy = null;
    this.pendingPreview = null;
    this.dispatch(epoch, { type: 'scan-saved', scanId: pending.scanId });
    let occupancyBytes: Uint8Array | null = null;
    try {
      occupancyBytes = pending.buildOccupancy().slice();
      this.pendingOccupancy = { objectId: pending.objectId, sessionId: pending.sessionId, scanId: pending.scanId, bytes: occupancyBytes };
    } catch (reason) {
      this.dispatch(epoch, { type: 'occupancy-failed', error: `Occupancy could not be prepared: ${errorText(reason)}` });
    }
    try {
      const preview = pending.buildPreview(occupancyBytes).slice();
      this.pendingPreview = { objectId: pending.objectId, sessionId: pending.sessionId, scanId: pending.scanId, bytes: preview };
    } catch (reason) {
      this.dispatch(epoch, { type: 'preview-failed', error: `Preview could not be prepared: ${errorText(reason)}` });
    }
    if (this.pendingOccupancy === null && this.pendingPreview === null) return this.state;
    if (this.pendingScan === pending && this.pendingOccupancy !== null && this.pendingPreview !== null) this.pendingScan = null;
    return this.pendingOccupancy === null ? this.uploadPreview(epoch) : this.uploadOccupancy(epoch);
  }

  async retry(objectId: string, sessionId: string): Promise<ScanPersistenceState | null> {
    const epoch = ++this.epoch;
    const pendingScan = this.pendingScan;
    if (pendingScan !== null && pendingScan.objectId === objectId && pendingScan.sessionId === sessionId &&
        (this.state?.scan === 'blocked_recording' || this.state?.scan === 'error')) {
      this.dispatch(epoch, { type: 'scan-saving', scanId: pendingScan.scanId });
      return this.uploadScan(epoch, pendingScan);
    }
    if (pendingScan !== null && pendingScan.objectId === objectId && pendingScan.sessionId === sessionId &&
        (this.state?.scan === 'unknown' || this.state?.scan === 'verifying')) {
      return this.verifyUnknown(epoch, pendingScan, this.state.scanError ?? 'Scan save outcome is still unknown.');
    }
    if (this.state?.scan !== 'saved') return this.state;
    if (this.state.occupancy === 'failed' || this.state.occupancy === 'stale') {
      if (this.pendingOccupancy === null && pendingScan !== null) this.prepareOccupancy(epoch, pendingScan);
      if (this.pendingOccupancy !== null && this.pendingOccupancy.objectId === objectId && this.pendingOccupancy.sessionId === sessionId) {
        return this.uploadOccupancy(epoch);
      }
    }
    if ((this.state.preview === 'failed' || this.state.preview === 'stale') && this.pendingPreview === null && pendingScan !== null) {
      this.preparePreview(epoch, pendingScan, this.pendingOccupancy?.bytes ?? null);
    }
    if ((this.state.preview === 'failed' || this.state.preview === 'stale') && this.pendingPreview !== null &&
        this.pendingPreview.objectId === objectId && this.pendingPreview.sessionId === sessionId) return this.uploadPreview(epoch);
    return this.state;
  }

  async reconcile(
    objectId: string,
    sessionId: string,
    savedScanBytes: Uint8Array | null,
    buildOccupancy?: () => Uint8Array,
    buildPreview?: (occupancyBytes: Uint8Array | null) => Uint8Array,
    loadedScanId?: string,
  ): Promise<ScanPersistenceState | null> {
    const epoch = ++this.epoch;
    let scanInfo: ScanArtifactInfo;
    let occupancyInfo: ScanArtifactInfo;
    let previewInfo: ScanArtifactInfo;
    try {
      [scanInfo, occupancyInfo, previewInfo] = await Promise.all([
        this.readInfo(objectId, sessionId, 'scan'),
        this.readInfo(objectId, sessionId, 'occupancy'),
        this.readInfo(objectId, sessionId, 'preview'),
      ]);
    } catch (reason) {
      if (epoch === this.epoch) this.dispatch(epoch, { type: 'reconcile-unknown', scanId: null, error: `Could not verify scan persistence: ${errorText(reason)}` });
      return this.state;
    }
    if (epoch !== this.epoch) return this.state;
    const key = `${objectId}/${sessionId}`;
    if (!scanInfo.present) {
      this.knownScan = { key, identity: infoIdentity(scanInfo) };
      this.pendingOccupancy = null;
      this.pendingPreview = null;
      this.pendingScan = null;
      this.state = null;
      this.onState(null);
      return null;
    }
    const scanId = scanInfo.id ?? null;
    if (scanId === null) {
      this.knownScan = { key, identity: infoIdentity(scanInfo) };
      this.dispatch(epoch, { type: 'reconcile-unknown', scanId: null, error: 'The saved scan has no readable generation identity.' });
      return this.state;
    }
    const bytesId = savedScanBytes === null ? null : loadedScanId ?? occupancyFrameIdForScan(savedScanBytes);
    if (bytesId !== null && (bytesId !== scanId || scanInfo.bytes !== savedScanBytes?.byteLength)) {
      this.knownScan = { key, identity: infoIdentity(scanInfo) };
      this.dispatch(epoch, { type: 'reconcile-unknown', scanId, error: 'Loaded scan bytes do not match the current device generation.' });
      return this.state;
    }
    this.knownScan = { key, identity: infoIdentity(scanInfo) };
    this.dispatch(epoch, { type: 'reconcile-saved', scanId });

    const occupancyMatches = occupancyInfo.present && occupancyInfo.sourceId != null && occupancyInfo.sourceId === scanId;
    const previewMatches = previewInfo.present && previewInfo.sourceId != null && previewInfo.sourceId === scanId;
    const occupancyNeedsBuild = !occupancyMatches;
    const previewNeedsBuild = !previewMatches;
    const canBuild = savedScanBytes !== null && bytesId === scanId &&
      scanInfo.bytes === savedScanBytes.byteLength && buildOccupancy !== undefined && buildPreview !== undefined;
    const matchingPendingOccupancy = this.pendingOccupancy?.objectId === objectId &&
      this.pendingOccupancy.sessionId === sessionId && this.pendingOccupancy.scanId === scanId;
    const matchingPendingPreview = this.pendingPreview?.objectId === objectId &&
      this.pendingPreview.sessionId === sessionId && this.pendingPreview.scanId === scanId;
    let pending = this.pendingScan;
    if (canBuild && (occupancyNeedsBuild || previewNeedsBuild)) {
      pending = {
        objectId,
        sessionId,
        bytes: savedScanBytes!.slice(),
        scanId,
        previous: infoIdentity(scanInfo),
        previousPersistence: this.state,
        buildOccupancy,
        buildPreview,
      };
      this.pendingScan = pending;
    }
    if (occupancyMatches) this.dispatch(epoch, { type: 'reconcile-occupancy-ready' });
    else if (pending !== null && pending.objectId === objectId && pending.sessionId === sessionId && pending.scanId === scanId) {
      if (occupancyInfo.present) {
        this.prepareOccupancy(epoch, pending);
        this.dispatch(epoch, { type: 'reconcile-occupancy-stale', error: 'Occupancy belongs to a different scan generation; rebuild it for the current scan.' });
      } else if (matchingPendingOccupancy) {
        this.dispatch(epoch, { type: 'reconcile-occupancy-missing-retryable' });
      } else {
        this.prepareOccupancy(epoch, pending);
        if (this.pendingOccupancy === null) this.dispatch(epoch, { type: 'reconcile-occupancy-unavailable', error: 'Occupancy is missing and could not be rebuilt from the loaded scan.' });
        else this.dispatch(epoch, { type: 'reconcile-occupancy-missing-retryable' });
      }
    } else if (occupancyInfo.present) {
      this.dispatch(epoch, { type: 'reconcile-occupancy-stale', error: 'Occupancy belongs to a different scan generation.' });
    } else if (matchingPendingOccupancy) {
      this.dispatch(epoch, { type: 'reconcile-occupancy-missing-retryable' });
    } else {
      this.dispatch(epoch, { type: 'reconcile-occupancy-unavailable', error: 'Occupancy is missing and matching bytes are not in memory. Rescan and re-save the scan to create them safely.' });
    }

    if (previewMatches) this.dispatch(epoch, { type: 'reconcile-preview-ready' });
    else if (pending !== null && pending.objectId === objectId && pending.sessionId === sessionId && pending.scanId === scanId) {
      if (previewInfo.present) {
        this.preparePreview(epoch, pending, this.pendingOccupancy?.bytes ?? null);
        this.dispatch(epoch, { type: 'reconcile-preview-stale', error: 'Preview belongs to a different scan generation; rebuild it for the current scan.' });
      } else if (matchingPendingPreview) {
        this.dispatch(epoch, { type: 'reconcile-preview-missing-retryable' });
      } else {
        this.preparePreview(epoch, pending, this.pendingOccupancy?.bytes ?? null);
        if (this.pendingPreview === null) this.dispatch(epoch, { type: 'reconcile-preview-unavailable', error: 'Preview is missing and could not be rebuilt from the loaded scan.' });
        else this.dispatch(epoch, { type: 'reconcile-preview-missing-retryable' });
      }
    } else if (previewInfo.present) {
      this.dispatch(epoch, { type: 'reconcile-preview-stale', error: 'Preview belongs to a different scan generation.' });
    } else if (matchingPendingPreview) {
      this.dispatch(epoch, { type: 'reconcile-preview-missing-retryable' });
    } else {
      this.dispatch(epoch, { type: 'reconcile-preview-unavailable', error: 'Preview is missing and matching bytes are not in memory. Rescan and re-save the scan to create them safely.' });
    }
    if (this.pendingScan === pending && this.pendingOccupancy !== null && this.pendingPreview !== null) this.pendingScan = null;
    return this.state;
  }

  private prepareOccupancy(epoch: number, pending: PendingScan): void {
    try {
      const bytes = pending.buildOccupancy().slice();
      this.pendingOccupancy = { objectId: pending.objectId, sessionId: pending.sessionId, scanId: pending.scanId, bytes };
    } catch (reason) {
      this.pendingOccupancy = null;
      if (this.state?.scan === 'saved' && this.state.occupancy !== 'ready') {
        this.dispatch(epoch, { type: 'occupancy-failed', error: `Occupancy could not be prepared: ${errorText(reason)}` });
      }
    }
  }

  private preparePreview(epoch: number, pending: PendingScan, occupancyBytes: Uint8Array | null): void {
    try {
      const bytes = pending.buildPreview(occupancyBytes).slice();
      this.pendingPreview = { objectId: pending.objectId, sessionId: pending.sessionId, scanId: pending.scanId, bytes };
    } catch (reason) {
      this.pendingPreview = null;
      if (this.state?.scan === 'saved' && this.state.preview !== 'ready') {
        this.dispatch(epoch, { type: 'preview-failed', error: `Preview could not be prepared: ${errorText(reason)}` });
      }
    }
  }

  private async uploadOccupancy(epoch: number): Promise<ScanPersistenceState | null> {
    const pending = this.pendingOccupancy;
    if (pending === null) return this.uploadPreview(epoch);
    this.dispatch(epoch, { type: 'occupancy-saving' });
    try {
      const result = await this.ports.saveOccupancy(pending.objectId, pending.sessionId, pending.bytes, pending.scanId);
      if (epoch !== this.epoch) return this.state;
      if (generationMismatchResult(result)) throw new ScanWriteOutcomeError('confirmed_not_committed', 'scan_generation_mismatch', 409, result.current_id ?? null);
      requireAccepted(result);
      this.pendingOccupancy = null;
      this.dispatch(epoch, { type: 'occupancy-ready' });
    } catch (reason) {
      if (epoch !== this.epoch) return this.state;
      if (isGenerationMismatch(reason)) {
        this.knownScan = {
          key: `${pending.objectId}/${pending.sessionId}`,
          identity: { known: true, present: reason.currentId !== null, id: reason.currentId },
        };
        this.pendingOccupancy = null;
        this.pendingPreview = null;
        this.pendingScan = null;
        this.dispatch(epoch, { type: 'scan-derived-stale', scanId: pending.scanId, error: errorText(reason) });
        return this.state;
      }
      this.dispatch(epoch, { type: 'occupancy-failed', error: errorText(reason) });
      if (reason instanceof BulkTransferPreemptedError) return this.state;
    }
    if (epoch !== this.epoch) return this.state;
    return this.uploadPreview(epoch);
  }

  private async uploadPreview(epoch: number): Promise<ScanPersistenceState | null> {
    const pending = this.pendingPreview;
    if (pending === null) return this.state;
    this.dispatch(epoch, { type: 'preview-saving' });
    try {
      const result = await this.ports.savePreview(pending.objectId, pending.sessionId, pending.bytes, pending.scanId);
      if (epoch !== this.epoch) return this.state;
      if (generationMismatchResult(result)) throw new ScanWriteOutcomeError('confirmed_not_committed', 'scan_generation_mismatch', 409, result.current_id ?? null);
      requireAccepted(result);
      this.pendingPreview = null;
      this.dispatch(epoch, { type: 'preview-ready' });
    } catch (reason) {
      if (epoch !== this.epoch) return this.state;
      if (isGenerationMismatch(reason)) {
        this.knownScan = {
          key: `${pending.objectId}/${pending.sessionId}`,
          identity: { known: true, present: reason.currentId !== null, id: reason.currentId },
        };
        this.pendingOccupancy = null;
        this.pendingPreview = null;
        this.pendingScan = null;
        this.dispatch(epoch, { type: 'scan-derived-stale', scanId: pending.scanId, error: errorText(reason) });
        return this.state;
      }
      this.dispatch(epoch, { type: 'preview-failed', error: errorText(reason) });
    }
    return this.state;
  }

  private readInfo(objectId: string, sessionId: string, kind: 'scan' | 'occupancy' | 'preview'): Promise<ScanArtifactInfo> {
    return retryPreemptedRead(() => this.ports.fetchInfo(objectId, sessionId, kind));
  }

  private dispatch(epoch: number, event: ScanPersistenceEvent): void {
    if (epoch !== this.epoch) return;
    this.state = transitionScanPersistence(this.state, event);
    this.onState(this.state);
  }
}
