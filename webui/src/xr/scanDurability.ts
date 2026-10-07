export type ScanDurabilityPhase = 'none' | 'durable' | 'provisional' | 'verifying' | 'unknown' | 'not_committed';
export type ScanViewportProvenance = 'none' | 'durable' | 'provisional';
export type ScanCaptureIntent = 'fresh' | 'continue';

export interface ScanDurabilityState<Snapshot> {
  phase: ScanDurabilityPhase;
  captureActive: boolean;
  captureIntent: ScanCaptureIntent | null;
  durableSnapshot: Snapshot | null;
  durableId: string | null;
  provisionalSnapshot: Snapshot | null;
  candidateId: string | null;
  previousId: string | null;
  previousKept: boolean;
  derivativesReady: boolean;
}

export function emptyScanDurability<Snapshot>(): ScanDurabilityState<Snapshot> {
  return {
    phase: 'none',
    captureActive: false,
    captureIntent: null,
    durableSnapshot: null,
    durableId: null,
    provisionalSnapshot: null,
    candidateId: null,
    previousId: null,
    previousKept: false,
    derivativesReady: false,
  };
}

/** Keep the last confirmed scan intact while a WebXR capture is starting. */
export function captureStarted<Snapshot>(
  state: ScanDurabilityState<Snapshot>,
  intent: ScanCaptureIntent,
): ScanDurabilityState<Snapshot> {
  return { ...state, captureActive: true, captureIntent: intent };
}

/** End an unsuccessful or interrupted capture without changing scan durability. */
export function captureAborted<Snapshot>(state: ScanDurabilityState<Snapshot>): ScanDurabilityState<Snapshot> {
  if (!state.captureActive) return state;
  return { ...state, captureActive: false, captureIntent: null };
}

export function beginProvisionalScan<Snapshot>(
  state: ScanDurabilityState<Snapshot>,
  snapshot: Snapshot,
  candidateId: string,
): ScanDurabilityState<Snapshot> {
  return {
    ...state,
    phase: 'provisional',
    captureActive: false,
    captureIntent: null,
    provisionalSnapshot: snapshot,
    candidateId,
    previousId: state.durableId,
    previousKept: false,
  };
}

export function markScanVerifying<Snapshot>(state: ScanDurabilityState<Snapshot>): ScanDurabilityState<Snapshot> {
  return state.provisionalSnapshot === null ? state : { ...state, phase: 'verifying', previousKept: false };
}

export function markScanUnknown<Snapshot>(state: ScanDurabilityState<Snapshot>): ScanDurabilityState<Snapshot> {
  if (state.provisionalSnapshot !== null) {
    return {
      ...state,
      phase: 'unknown',
      captureActive: false,
      captureIntent: null,
      previousKept: false,
      derivativesReady: false,
    };
  }
  return {
    ...state,
    phase: 'unknown',
    captureActive: false,
    captureIntent: null,
    provisionalSnapshot: state.durableSnapshot,
    candidateId: state.durableId,
    previousKept: false,
    derivativesReady: false,
  };
}

export function confirmScanCommitted<Snapshot>(
  state: ScanDurabilityState<Snapshot>,
  candidateId: string,
): ScanDurabilityState<Snapshot> {
  if (state.provisionalSnapshot === null || state.candidateId !== candidateId) return state;
  return {
    ...state,
    phase: 'durable',
    captureActive: false,
    captureIntent: null,
    durableSnapshot: state.provisionalSnapshot,
    durableId: candidateId,
    provisionalSnapshot: null,
    candidateId: null,
    previousId: null,
    previousKept: false,
    derivativesReady: false,
  };
}

export function confirmScanNotCommitted<Snapshot>(
  state: ScanDurabilityState<Snapshot>,
  previousKept: boolean,
): ScanDurabilityState<Snapshot> {
  return {
    ...state,
    phase: 'not_committed',
    captureActive: false,
    captureIntent: null,
    previousKept,
    derivativesReady: state.durableSnapshot !== null && state.derivativesReady,
  };
}

export function reconcileDurableScan<Snapshot>(
  state: ScanDurabilityState<Snapshot>,
  snapshot: Snapshot,
  scanId: string,
  derivativesReady: boolean,
): ScanDurabilityState<Snapshot> {
  return {
    ...state,
    phase: 'durable',
    captureActive: false,
    captureIntent: null,
    durableSnapshot: snapshot,
    durableId: scanId,
    provisionalSnapshot: null,
    candidateId: null,
    previousId: null,
    previousKept: false,
    derivativesReady,
  };
}

export function setScanDerivativesReady<Snapshot>(
  state: ScanDurabilityState<Snapshot>,
  ready: boolean,
): ScanDurabilityState<Snapshot> {
  return { ...state, derivativesReady: ready };
}

export function previousKeptProvable<Snapshot>(state: ScanDurabilityState<Snapshot>): boolean {
  return state.phase === 'not_committed' && state.previousKept && state.durableSnapshot !== null &&
    state.previousId !== null && state.durableId === state.previousId;
}

/** The last confirmed scan remains authoritative until a replacement is confirmed. */
export function authoritativeSnapshot<Snapshot>(state: ScanDurabilityState<Snapshot>): Snapshot | null {
  if (state.phase === 'durable' || state.phase === 'provisional' || state.phase === 'verifying') {
    return state.durableSnapshot;
  }
  if (previousKeptProvable(state)) return state.durableSnapshot;
  return null;
}

export function savedScanAvailable<Snapshot>(state: ScanDurabilityState<Snapshot>): boolean {
  return state.durableSnapshot !== null &&
    (state.phase === 'durable' || previousKeptProvable(state));
}

export function continueAvailable<Snapshot>(state: ScanDurabilityState<Snapshot>): boolean {
  return savedScanAvailable(state);
}

export function shouldShowScanningPresentation<Snapshot>(state: ScanDurabilityState<Snapshot>): boolean {
  return state.phase === 'none' && state.durableSnapshot === null && state.provisionalSnapshot === null;
}

export function resultControlsAvailable<Snapshot>(state: ScanDurabilityState<Snapshot>): boolean {
  return viewportSnapshot(state) !== null;
}

export function localizationReady<Snapshot>(state: ScanDurabilityState<Snapshot>): boolean {
  return (state.phase === 'durable' || previousKeptProvable(state)) &&
    state.durableSnapshot !== null && state.derivativesReady;
}

export function viewportSnapshot<Snapshot>(state: ScanDurabilityState<Snapshot>): Snapshot | null {
  if ((state.phase === 'provisional' || state.phase === 'verifying' || state.phase === 'unknown') &&
      state.provisionalSnapshot !== null) return state.provisionalSnapshot;
  if (state.phase === 'not_committed' && !previousKeptProvable(state) && state.provisionalSnapshot !== null) return state.provisionalSnapshot;
  return state.durableSnapshot ?? state.provisionalSnapshot;
}

export function viewportProvenance<Snapshot>(state: ScanDurabilityState<Snapshot>): ScanViewportProvenance {
  if (state.durableSnapshot === null && state.provisionalSnapshot === null) return 'none';
  if (((state.phase === 'provisional' || state.phase === 'verifying' || state.phase === 'unknown') ||
      (state.phase === 'not_committed' && !previousKeptProvable(state))) && state.provisionalSnapshot !== null) {
    return 'provisional';
  }
  return state.durableSnapshot !== null ? 'durable' : 'provisional';
}
