import {
  BulkTransferPreemptedError,
  retryPreemptedRead,
  ScanWriteOutcomeError,
  type ControlResult,
  type ScanArtifactInfo,
} from '../api/deviceApi';
import { createScanIdentity, recordControllerOwnedScanCopy, type ScanIdentity } from './scanIdentity';

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
  saveOccupancy(objectId: string, sessionId: string, bytes: Uint8Array, scanId?: string, scanSha256?: string): Promise<ControlResult>;
  savePreview(objectId: string, sessionId: string, bytes: Uint8Array, scanId?: string, scanSha256?: string): Promise<ControlResult>;
  fetchInfo(objectId: string, sessionId: string, kind: 'scan' | 'occupancy' | 'preview'): Promise<ScanArtifactInfo>;
}

export function scanPersistenceSessionId(sessionId: string | null): string {
  return sessionId ?? 'staging';
}

interface KnownScanIdentity {
  known: boolean;
  present: boolean;
  id: string | null;
}

interface PendingOccupancy {
  objectId: string;
  sessionId: string;
  scanId: string;
  /** SHA-256 of the scan this derivative was built for; the device refuses it for any other scan, even one with the same CRC32. */
  scanSha256?: string;
  bytes: Uint8Array;
}

interface PendingPreview {
  objectId: string;
  sessionId: string;
  scanId: string;
  scanSha256?: string;
  /** The preview was built together with an occupancy artifact; false = from fallback surface samples, to be rebuilt once an occupancy exists. */
  fromOccupancy: boolean;
  bytes: Uint8Array;
}

/**
 * Derivative builders may be asynchronous (MAX-403): a registered derivative can wait for the
 * bounded tag-finalization window. The scan upload NEVER waits for a builder. Since issue #44 the builders start together with the save (so the
 * operator can inspect the exact bytes that will be sent while the upload is still pending); the commit reuses what they produced and only
 * rebuilds a derivative whose early attempt failed.
 */
/** The derivative bytes prepared for one scan; the very arrays that are uploaded, not copies. `null` = not (yet) prepared. */
export interface PreparedDerivatives {
  objectId: string;
  sessionId: string;
  scanId: string;
  scanSha256?: string;
  occupancy: Uint8Array | null;
  preview: Uint8Array | null;
}
export type DerivativeBytes = Uint8Array | Promise<Uint8Array>;

function abandonedDerivativeBuilder(): never {
  throw new Error('Scan persistence owner was abandoned.');
}

function isPromiseLike(value: DerivativeBytes): value is Promise<Uint8Array> {
  return typeof (value as { then?: unknown }).then === 'function';
}

interface PendingScan {
  objectId: string;
  sessionId: string;
  /** A single owned payload is retained only until the primary scan is durable. */
  bytes: Uint8Array | null;
  identity: ScanIdentity;
  scanId: string;
  previous: KnownScanIdentity;
  previousPersistence: ScanPersistenceState | null;
  buildOccupancy: () => DerivativeBytes;
  buildPreview: (occupancyBytes: Uint8Array | null) => DerivativeBytes;
  /** Settles when the early derivative preparation (issue #44) is over, whatever its outcome. */
  early?: Promise<void>;
}

function errorText(reason: unknown): string {
  return reason instanceof Error ? reason.message : String(reason);
}

function infoIdentity(info: ScanArtifactInfo): KnownScanIdentity {
  return { known: true, present: info.present, id: info.id ?? null };
}

function sameScan(left: KnownScanIdentity, right: ScanArtifactInfo): boolean {
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

/**
 * Scan byte ownership:
 * - Retry payload: this controller's one synchronous copy in `pendingScan.bytes`.
 * - Fetch copies BufferSource bodies internally, so it does not detach the controller's retained retry buffer. Revisit this assumption before using ReadableStream or transferred bodies.
 * - The controller drops retry bytes on confirmed commit, abandon, reset, or generation mismatch; fetch manages its internal request copy.
 * - Derivatives: each pending slot owns its builder's bytes until that upload succeeds or the owner is abandoned.
 */
export class ScanPersistenceController {
  private state: ScanPersistenceState | null = null;
  private pendingOccupancy: PendingOccupancy | null = null;
  private pendingPreview: PendingPreview | null = null;
  private pendingScan: PendingScan | null = null;
  private knownScan: { key: string; identity: KnownScanIdentity } | null = null;
  private epoch = 0;
  private prepared: PreparedDerivatives | null = null;

  constructor(
    private readonly ports: ScanPersistencePorts,
    private readonly onState: (state: ScanPersistenceState | null) => void = () => undefined,
    private readonly onPrepared: (prepared: PreparedDerivatives | null) => void = () => undefined,
  ) {}

  /** The derivatives prepared for the scan being saved, available before anything is sent. */
  get preparedDerivatives(): PreparedDerivatives | null { return this.prepared; }

  private publishPrepared(next: PreparedDerivatives | null): void {
    this.prepared = next;
    this.onPrepared(next);
  }

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
    this.abandon();
    this.state = null;
    this.onState(null);
  }

  /** Cancel derivative work owned by an unmounted scanner without publishing state. */
  abandon(): void {
    this.epoch += 1;
    this.pendingOccupancy = null;
    this.pendingPreview = null;
    if (this.prepared !== null) this.publishPrepared(null);
    if (this.pendingScan !== null) {
      this.pendingScan.bytes = null;
      this.pendingScan.buildOccupancy = abandonedDerivativeBuilder;
      this.pendingScan.buildPreview = abandonedDerivativeBuilder;
    }
    this.pendingScan = null;
    this.knownScan = null;
  }

  async save(
    objectId: string,
    sessionId: string,
    scanBytes: Uint8Array,
    buildOccupancy: () => DerivativeBytes,
    buildPreview: (occupancyBytes: Uint8Array | null) => DerivativeBytes,
    blockedByRecording = false,
    candidateId?: string,
    suppliedIdentity?: ScanIdentity,
  ): Promise<ScanPersistenceState | null> {
    const identity = suppliedIdentity ?? createScanIdentity(scanBytes);
    if (identity.byteLength !== scanBytes.byteLength || (candidateId !== undefined && candidateId !== identity.frameId)) {
      throw new Error('Scan identity does not match the encoded scan bytes.');
    }
    const epoch = ++this.epoch;
    this.pendingOccupancy = null;
    this.pendingPreview = null;
    if (this.prepared !== null) this.publishPrepared(null);
    const copiedByteLength = scanBytes.byteLength;
    const key = `${objectId}/${sessionId}`;
    const pending: PendingScan = {
      objectId,
      sessionId,
      bytes: scanBytes.slice(),
      identity,
      scanId: candidateId ?? identity.frameId,
      previous: this.knownScan?.key === key ? this.knownScan.identity : { known: false, present: false, id: null },
      previousPersistence: this.state,
      buildOccupancy,
      buildPreview,
    };
    recordControllerOwnedScanCopy(copiedByteLength);
    scanBytes = new Uint8Array();
    this.pendingScan = pending;
    this.dispatch(epoch, { type: 'scan-saving', scanId: pending.scanId });
    if (blockedByRecording) {
      this.dispatch(epoch, { type: 'scan-blocked-recording', scanId: pending.scanId });
      this.startEarlyPreparation(epoch, pending);
      return this.state;
    }
    /* The upload starts first and parks on its identity lookup; the builders run in that window and never delay it (MAX-403). */
    const uploading = this.prepareAndUploadScan(epoch, pending);
    this.startEarlyPreparation(epoch, pending);
    return uploading;
  }

  /**
   * Issue #44: build the derivatives as soon as the scan is handed over, so the exact bytes that will be uploaded can be inspected before any of them is
   * sent. A failed early attempt is not an error yet: the commit repeats it exactly as it did before, and reports a failure then.
   */
  private startEarlyPreparation(epoch: number, pending: PendingScan): void {
    const run = async (): Promise<void> => {
      const occupancyWait = this.prepareOccupancy(epoch, pending);
      if (occupancyWait !== undefined) await occupancyWait;
      if (epoch !== this.epoch) return;
      const occupancy = this.pendingOccupancy as PendingOccupancy | null;
      this.publishPreparedFor(pending, occupancy?.bytes ?? null, null);
      const previewWait = this.preparePreview(epoch, pending, occupancy?.bytes ?? null);
      if (previewWait !== undefined) await previewWait;
      if (epoch !== this.epoch) return;
      const preview = this.pendingPreview as PendingPreview | null;
      this.publishPreparedFor(pending, occupancy?.bytes ?? null, preview?.bytes ?? null);
    };
    pending.early = run().catch(() => undefined);
  }

  private publishPreparedFor(pending: PendingScan, occupancy: Uint8Array | null, preview: Uint8Array | null): void {
    if (occupancy === null && preview === null) return;
    this.publishPrepared({ objectId: pending.objectId, sessionId: pending.sessionId, scanId: pending.scanId, scanSha256: pending.identity.sha256, occupancy, preview });
  }

  private async prepareAndUploadScan(epoch: number, pending: PendingScan): Promise<ScanPersistenceState | null> {
    if (!pending.previous.known) {
      try {
        const previous = infoIdentity(await this.readInfo(pending.objectId, pending.sessionId, 'scan'));
        if (epoch !== this.epoch) return this.state;
        pending.previous = previous;
        this.knownScan = { key: `${pending.objectId}/${pending.sessionId}`, identity: pending.previous };
      } catch {
        if (epoch !== this.epoch) return this.state;
        pending.previous = { known: false, present: false, id: null };
      }
    }
    return this.uploadScan(epoch, pending);
  }

  private async uploadScan(epoch: number, pending: PendingScan): Promise<ScanPersistenceState | null> {
    if (pending.bytes === null) return this.state;
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
    pending.bytes = null;
    this.knownScan = { key: `${pending.objectId}/${pending.sessionId}`, identity: { known: true, present: true, id: pending.scanId } };
    /* The early attempt (issue #44) may still be running or may have produced the bytes the operator is already looking at: use them, do not rebuild. */
    if (pending.early === undefined) {
      this.pendingOccupancy = null;
      this.pendingPreview = null;
    }
    this.dispatch(epoch, { type: 'scan-saved', scanId: pending.scanId });
    if (pending.early !== undefined) {
      await pending.early;
      if (epoch !== this.epoch) return this.state;
    }
    if (this.pendingOccupancy === null) {
      const occupancyWait = this.prepareOccupancy(epoch, pending);
      if (occupancyWait !== undefined) {
        await occupancyWait;
        if (epoch !== this.epoch) return this.state;
      }
    }
    // The builders above may have assigned these fields while awaited; read them through the declared type.
    const preparedOccupancy = this.pendingOccupancy as PendingOccupancy | null;
    /* A preview built while the occupancy had failed came from fallback samples; once the occupancy exists it must be rebuilt from it, or the pair disagrees. */
    const previewBeforeOccupancy = this.pendingPreview as PendingPreview | null;
    if (previewBeforeOccupancy !== null && !previewBeforeOccupancy.fromOccupancy && preparedOccupancy !== null) this.pendingPreview = null;
    if (this.pendingPreview === null) {
      const previewWait = this.preparePreview(epoch, pending, preparedOccupancy?.bytes ?? null);
      if (previewWait !== undefined) {
        await previewWait;
        if (epoch !== this.epoch) return this.state;
      }
    }
    this.publishPreparedFor(pending, preparedOccupancy?.bytes ?? null, (this.pendingPreview as PendingPreview | null)?.bytes ?? null);
    if (this.pendingOccupancy === null && this.pendingPreview === null) return this.state;
    if (this.pendingScan === pending && this.pendingOccupancy !== null && this.pendingPreview !== null) this.pendingScan = null;
    return this.pendingOccupancy === null ? this.uploadPreview(epoch) : this.uploadOccupancy(epoch);
  }

  async retry(objectId: string, sessionId: string): Promise<ScanPersistenceState | null> {
    const epoch = ++this.epoch;
    const pendingScan = this.pendingScan;
    if (pendingScan !== null && pendingScan.bytes !== null && pendingScan.objectId === objectId && pendingScan.sessionId === sessionId &&
        (this.state?.scan === 'blocked_recording' || this.state?.scan === 'error')) {
      this.dispatch(epoch, { type: 'scan-saving', scanId: pendingScan.scanId });
      return this.uploadScan(epoch, pendingScan);
    }
    if (pendingScan !== null && pendingScan.bytes !== null && pendingScan.objectId === objectId && pendingScan.sessionId === sessionId &&
        (this.state?.scan === 'unknown' || this.state?.scan === 'verifying')) {
      return this.verifyUnknown(epoch, pendingScan, this.state.scanError ?? 'Scan save outcome is still unknown.');
    }
    if (this.state?.scan !== 'saved') return this.state;
    if (this.state.occupancy === 'failed' || this.state.occupancy === 'stale') {
      if (this.pendingOccupancy === null && pendingScan !== null) {
        const wait = this.prepareOccupancy(epoch, pendingScan);
        if (wait !== undefined) {
          await wait;
          if (epoch !== this.epoch) return this.state;
        }
      }
      if (this.pendingOccupancy !== null && this.pendingOccupancy.objectId === objectId && this.pendingOccupancy.sessionId === sessionId) {
        return this.uploadOccupancy(epoch);
      }
    }
    if ((this.state.preview === 'failed' || this.state.preview === 'stale') && this.pendingPreview === null && pendingScan !== null) {
      const wait = this.preparePreview(epoch, pendingScan, this.pendingOccupancy?.bytes ?? null);
      if (wait !== undefined) {
        await wait;
        if (epoch !== this.epoch) return this.state;
      }
    }
    if ((this.state.preview === 'failed' || this.state.preview === 'stale') && this.pendingPreview !== null &&
        this.pendingPreview.objectId === objectId && this.pendingPreview.sessionId === sessionId) return this.uploadPreview(epoch);
    return this.state;
  }

  async reconcile(
    objectId: string,
    sessionId: string,
    savedScanBytes: Uint8Array | null,
    buildOccupancy?: () => DerivativeBytes,
    buildPreview?: (occupancyBytes: Uint8Array | null) => DerivativeBytes,
    loadedScanId?: string,
    loadedScanIdentity?: ScanIdentity,
  ): Promise<ScanPersistenceState | null> {
    const loadedScanByteLength = savedScanBytes?.byteLength ?? loadedScanIdentity?.byteLength ?? null;
    const loadedIdentity = loadedScanIdentity ?? (savedScanBytes === null ? null : createScanIdentity(savedScanBytes));
    const identityMatchesBytes = savedScanBytes === null || loadedIdentity?.byteLength === savedScanBytes.byteLength;
    savedScanBytes = null;
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
    const bytesId = loadedIdentity?.frameId ?? loadedScanId ?? null;
    if (!identityMatchesBytes || (loadedIdentity !== null && loadedScanId !== undefined && loadedScanId !== loadedIdentity.frameId) ||
        (bytesId !== null && (bytesId !== scanId || scanInfo.bytes !== loadedScanByteLength))) {
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
    const canBuild = loadedIdentity !== null && bytesId === scanId &&
      scanInfo.bytes === loadedScanByteLength && buildOccupancy !== undefined && buildPreview !== undefined;
    const matchingPendingOccupancy = this.pendingOccupancy?.objectId === objectId &&
      this.pendingOccupancy.sessionId === sessionId && this.pendingOccupancy.scanId === scanId;
    const matchingPendingPreview = this.pendingPreview?.objectId === objectId &&
      this.pendingPreview.sessionId === sessionId && this.pendingPreview.scanId === scanId;
    let pending = this.pendingScan;
    if (canBuild && (occupancyNeedsBuild || previewNeedsBuild)) {
      pending = {
        objectId,
        sessionId,
        bytes: null,
        identity: loadedIdentity!,
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
        const wait = this.prepareOccupancy(epoch, pending);
        if (wait !== undefined) {
          await wait;
          if (epoch !== this.epoch) return this.state;
        }
        this.dispatch(epoch, { type: 'reconcile-occupancy-stale', error: 'Occupancy belongs to a different scan generation; rebuild it for the current scan.' });
      } else if (matchingPendingOccupancy) {
        this.dispatch(epoch, { type: 'reconcile-occupancy-missing-retryable' });
      } else {
        const wait = this.prepareOccupancy(epoch, pending);
        if (wait !== undefined) {
          await wait;
          if (epoch !== this.epoch) return this.state;
        }
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
        const wait = this.preparePreview(epoch, pending, this.pendingOccupancy?.bytes ?? null);
        if (wait !== undefined) {
          await wait;
          if (epoch !== this.epoch) return this.state;
        }
        this.dispatch(epoch, { type: 'reconcile-preview-stale', error: 'Preview belongs to a different scan generation; rebuild it for the current scan.' });
      } else if (matchingPendingPreview) {
        this.dispatch(epoch, { type: 'reconcile-preview-missing-retryable' });
      } else {
        const wait = this.preparePreview(epoch, pending, this.pendingOccupancy?.bytes ?? null);
        if (wait !== undefined) {
          await wait;
          if (epoch !== this.epoch) return this.state;
        }
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

  /** Returns a promise only for an asynchronous builder; a synchronous builder completes before returning. */
  private prepareOccupancy(epoch: number, pending: PendingScan): Promise<void> | undefined {
    const accept = (built: Uint8Array): void => {
      this.pendingOccupancy = { objectId: pending.objectId, sessionId: pending.sessionId, scanId: pending.scanId, scanSha256: pending.identity.sha256, bytes: built };
    };
    const fail = (reason: unknown): void => {
      this.pendingOccupancy = null;
      if (this.state?.scan === 'saved' && this.state.occupancy !== 'ready') {
        this.dispatch(epoch, { type: 'occupancy-failed', error: `Occupancy could not be prepared: ${errorText(reason)}` });
      }
    };
    let built: DerivativeBytes;
    try {
      built = pending.buildOccupancy();
      if (!isPromiseLike(built)) {
        accept(built);
        return undefined;
      }
    } catch (reason) {
      fail(reason);
      return undefined;
    }
    return built.then(
      (bytes) => {
        if (epoch !== this.epoch) return;
        try { accept(bytes); } catch (reason) { fail(reason); }
      },
      (reason: unknown) => {
        if (epoch !== this.epoch) return;
        fail(reason);
      },
    );
  }

  private preparePreview(epoch: number, pending: PendingScan, occupancyBytes: Uint8Array | null): Promise<void> | undefined {
    const accept = (built: Uint8Array): void => {
      this.pendingPreview = { objectId: pending.objectId, sessionId: pending.sessionId, scanId: pending.scanId, scanSha256: pending.identity.sha256, fromOccupancy: occupancyBytes !== null, bytes: built };
    };
    const fail = (reason: unknown): void => {
      this.pendingPreview = null;
      if (this.state?.scan === 'saved' && this.state.preview !== 'ready') {
        this.dispatch(epoch, { type: 'preview-failed', error: `Preview could not be prepared: ${errorText(reason)}` });
      }
    };
    let built: DerivativeBytes;
    try {
      built = pending.buildPreview(occupancyBytes);
      if (!isPromiseLike(built)) {
        accept(built);
        return undefined;
      }
    } catch (reason) {
      fail(reason);
      return undefined;
    }
    return built.then(
      (bytes) => {
        if (epoch !== this.epoch) return;
        try { accept(bytes); } catch (reason) { fail(reason); }
      },
      (reason: unknown) => {
        if (epoch !== this.epoch) return;
        fail(reason);
      },
    );
  }

  private async uploadOccupancy(epoch: number): Promise<ScanPersistenceState | null> {
    const pending = this.pendingOccupancy;
    if (pending === null) return this.uploadPreview(epoch);
    this.dispatch(epoch, { type: 'occupancy-saving' });
    try {
      const result = await this.ports.saveOccupancy(pending.objectId, pending.sessionId, pending.bytes, pending.scanId, pending.scanSha256);
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
      const result = await this.ports.savePreview(pending.objectId, pending.sessionId, pending.bytes, pending.scanId, pending.scanSha256);
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
