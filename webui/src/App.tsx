import { Component, lazy, Suspense, useCallback, useEffect, useMemo, useRef, useState, type ErrorInfo, type ReactNode } from 'react';

import './scan.css';

import {
  deleteObjectScanArtifact,
  fetchLocalizationRuns,
  fetchScanInfo,
  isRequestCancellation,
  deleteSessionRegistration,
  loadObjectScanArtifact,
  loadSessionRegistration,
  saveObjectScanArtifact,
  saveSessionRegistration,
  saveSessionOccupancyArtifact,
  saveSessionPreviewArtifact,
  retryPreemptedRead,
} from './api/deviceApi';
import type { LocalizationRunListing, LocalizationSessionGeometry } from './api/deviceApi';
import type { DeviceStatus } from './domain/device';
import { buildSessionOccupancyArtifact, decodeOccupancySolverArtifact, type DecodedOccupancyArtifact } from './xr/occupancySolverArtifact';
import { buildVoxelPreviewArtifact } from './xr/voxelPreview';
import { decodeRegistrationArtifact, encodeRegistrationArtifact, toOccupancyRegistration, type RegistrationArtifact } from './xr/fiducial/registrationArtifact';
import { TAG_FINALIZE_WINDOW_MS } from './xr/fiducial/tagModeState';
import {
  registrationApplicability,
  registrationBindingApplicability,
  REGISTRATION_INAPPLICABLE_KEYS,
  type RegistrationExpectation,
} from './xr/registrationApplicability';
import { bindRegistrationToScan, decideContinuationRegistration, type RegistrationAgreement } from './xr/registrationCompose';
import type { RigidTransform } from './xr/scanContinuation';
import { ScanPersistenceController, SCAN_STORAGE_BLOCKED_MESSAGE, scanPersistenceSessionId, type ScanPersistenceState } from './xr/scanPersistence';
import { scannerStorageActionsDisabled } from './features/objects/measurementTransition';
import type { ViewCommand, ViewPreset } from './components/SceneViewport';
import { XrRuntimePanel, type ScanStartIntent } from './components/XrRuntimePanel';
import { clientDiagnostics } from './data/clientDiagnostics';
import { countCoverageQualityBands } from './xr/coverageQuality';
import {
  deviceComputeProfilePromise,
  type DeviceComputeProfile,
} from './xr/deviceComputeProfile';
import {
  decodeScanArtifactWithIdentity,
  encodeScanArtifactWithIdentity,
  SCAN_ANNOTATION_CAPACITY_ERROR,
} from './xr/scanArtifact';
import { buildSensorPlacementScene } from './xr/sensorPlacementScene';
import { registerAndMergeScanContinuation } from './xr/scanContinuation';
import type { ScanIdentity } from './xr/scanIdentity';
import {
  beginProvisionalScan,
  captureAborted,
  captureStarted,
  authoritativeSnapshot,
  continueAvailable,
  confirmScanCommitted,
  confirmScanNotCommitted,
  emptyScanDurability,
  localizationReady,
  markScanUnknown,
  markScanVerifying,
  previousKeptProvable,
  reconcileDurableScan,
  resultControlsAvailable,
  savedScanAvailable,
  shouldShowScanningPresentation,
  setScanDerivativesReady,
  viewportProvenance,
  viewportSnapshot,
  type ScanDurabilityState,
} from './xr/scanDurability';
import {
  displayForScanMode,
  scanViewForMode,
  toggleScanLayer,
  type ScanDisplayOptions,
  type ScanMode,
} from './xr/scanDisplayModes';
import type { ScanPointCloudSnapshot, WebXrCapabilities } from './xr/webxrRuntime';
import { formatErrorText, TranslatedError, translate, useLocale, useT, type I18nLocale } from './i18n';
import { formatDistance, formatNumber } from './i18n/format';

const ScanPointCloudViewport = lazy(async () => {
  const module = await import('./components/ScanPointCloudViewport');
  return { default: module.ScanPointCloudViewport };
});

type ScanStoragePhase =
  | 'unbound'
  | 'loading'
  | 'none'
  | 'scanning'
  | 'saving'
  | 'saved'
  | 'loaded'
  | 'deleting'
  | 'error';

interface AppProps {
  objectId: string | null;
  /*
   * MAX-167: a 3D scan belongs to one recording/session, never directly to
   * the object. Every scan storage action is gated on both IDs together --
   * an object without an open session has nowhere valid to attach a new
   * scan, exactly like objectId === null before this change.
   */
  sessionId: string | null;
  status: DeviceStatus | null;
}

class ResultViewportErrorBoundary extends Component<
  { children: ReactNode; formatFailure: (error: string) => string },
  { message: string | null }
> {
  state: { message: string | null } = { message: null };

  static getDerivedStateFromError(error: unknown): { message: string } {
    return { message: error instanceof Error ? error.message : String(error) };
  }

  componentDidCatch(error: unknown, info: ErrorInfo): void {
    clientDiagnostics.warn('SCAN_RESULT_RENDERER_LOAD_FAILED', {
      error: error instanceof Error ? error.message : String(error),
      componentStack: info.componentStack ?? '',
    });
    void clientDiagnostics.flush();
  }

  render(): ReactNode {
    if (this.state.message !== null) {
      return (
        <div className="viewport-shell scan-viewport" role="alert">
          {this.props.formatFailure(this.state.message)}
        </div>
      );
    }
    return this.props.children;
  }
}

/** Wait for the final tag result slightly longer than the panel own bounded window (never delays the scan upload). */
const TAG_RESULT_WAIT_MS = TAG_FINALIZE_WINDOW_MS + 500;

type RegistrationStageState = 'idle' | 'pending' | 'saving' | 'saved' | 'failed';

/**
 * MAX-403/405/416: registration of one accepted scan candidate. It is resolved once (memoized) after
 * the bounded tag window and then feeds the derivative builders and the standalone registration
 * write, so occupancy, preview and registration-v1.json always agree about the same scan identity.
 */
interface ScanRegistrationStage {
  objectId: string;
  sessionId: string;
  intent: ScanStartIntent;
  scanId: string;
  base: RegistrationArtifact | null;
  extensionToBase: RigidTransform | null;
  tagFinal: boolean;
  tagResult: RegistrationArtifact | null;
  windowClosed: boolean;
  waiters: Array<() => void>;
  resolution: Promise<{ artifact: RegistrationArtifact | null; conflict: RegistrationAgreement | null }> | null;
  persistStarted: boolean;
}

function geometryGeneration(geometry: LocalizationSessionGeometry | null): number | null {
  if (geometry === null) return null;
  const value = Math.max(0, ...geometry.pods.filter((pod) => pod.computed).map((pod) => pod.computed_at_ms ?? 0));
  return value > 0 ? value : null;
}

function formatMeters(value: number, locale: I18nLocale): string {
  return formatDistance(locale, value, 2);
}

function extentFromBounds(
  bounds: ScanPointCloudSnapshot['bounds'],
): readonly [number, number, number] | null {
  if (bounds === null) return null;
  return [
    bounds.max[0] - bounds.min[0],
    bounds.max[1] - bounds.min[1],
    bounds.max[2] - bounds.min[2],
  ];
}

function computeProfileTitle(profile: DeviceComputeProfile, locale: I18nLocale): string {
  const capability = (available: boolean) => translate(locale, available ? 'scan.compute.available' : 'scan.compute.unavailable');
  return [
    translate(locale, 'scan.compute.logical_processors', { count: formatNumber(locale, profile.reportedLogicalProcessors) }),
    translate(locale, 'scan.compute.live_workers', { count: formatNumber(locale, profile.liveWorkerBudget) }),
    translate(locale, 'scan.compute.post_workers', { count: formatNumber(locale, profile.postScanWorkerBudget) }),
    translate(locale, 'scan.compute.shared_memory', { state: capability(profile.sharedMemory) }),
    translate(locale, 'scan.compute.wasm_threads', { state: capability(profile.wasmThreads) }),
    translate(locale, 'scan.compute.webgpu', { state: capability(profile.webGpu) }),
    translate(locale, 'scan.compute.tier', { tier: profile.tier }),
  ].join(' | ');
}

function scanStorageLabel(phase: ScanStoragePhase, objectId: string | null, locale: I18nLocale): string {
  if (phase === 'unbound') return translate(locale, 'scan.storage.unbound');
  const keys = {
    loading: 'scan.storage.loading', none: 'scan.storage.none', scanning: 'scan.storage.scanning',
    saving: 'scan.storage.saving', saved: 'scan.storage.saved', loaded: 'scan.storage.loaded',
    deleting: 'scan.storage.deleting', error: 'scan.storage.error',
  } as const;
  return translate(locale, keys[phase], {
    object: objectId ?? translate(locale, 'scan.storage.no_object'),
  });
}

function occupancyStatusLabel(persistence: ScanPersistenceState | null, locale: I18nLocale): string {
  if (persistence?.scan !== 'saved' && persistence?.scan !== 'derived_stale') return translate(locale, 'scan.occupancy.checking');
  const keys = {
    pending: 'scan.occupancy.pending', saving: 'scan.occupancy.saving', ready: 'scan.occupancy.ready',
    failed: 'scan.occupancy.failed', stale: 'scan.occupancy.stale', unavailable: 'scan.occupancy.unavailable',
  } as const;
  return translate(locale, keys[persistence.occupancy]);
}

function previewStatusLabel(persistence: ScanPersistenceState | null, locale: I18nLocale): string {
  if (persistence?.scan !== 'saved' && persistence?.scan !== 'derived_stale') return translate(locale, 'scan.preview.checking');
  const keys = {
    pending: 'scan.preview.pending', saving: 'scan.preview.saving', ready: 'scan.preview.ready',
    failed: 'scan.preview.failed', stale: 'scan.preview.stale', unavailable: 'scan.preview.unavailable',
  } as const;
  return translate(locale, keys[persistence.preview]);
}

function persistedScanStorageLabel(
  phase: ScanStoragePhase,
  objectId: string | null,
  persistence: ScanPersistenceState | null,
  locale: I18nLocale,
): string {
  if (persistence?.scan === 'verifying') return translate(locale, 'scan.storage.verifying');
  if (persistence?.scan === 'unknown' || persistence?.scan === 'derived_stale') return translate(locale, 'scan.storage.unknown');
  const scanLabel = scanStorageLabel(phase, objectId, locale);
  if (phase !== 'saved' && phase !== 'loaded') return scanLabel;
  return `${scanLabel} | ${occupancyStatusLabel(persistence, locale)}`;
}

export default function App({ objectId, sessionId, status }: AppProps) {
  const { locale } = useLocale();
  const t = useT();
  const scanSessionId = scanPersistenceSessionId(sessionId);
  const storageActionsDisabled = scannerStorageActionsDisabled(status);
  const [xrSessionActive, setXrSessionActive] = useState(false);
  const [xrCapabilities, setXrCapabilities] = useState<WebXrCapabilities | null>(null);
  const [computeProfile, setComputeProfile] = useState<DeviceComputeProfile | null>(null);
  const [scanDurability, setScanDurability] = useState<ScanDurabilityState<ScanPointCloudSnapshot>>(() => emptyScanDurability());
  const scanDurabilityRef = useRef(scanDurability);
  const activeScanCandidate = useRef<{ snapshot: ScanPointCloudSnapshot; id: string } | null>(null);
  const updateScanDurability = useCallback((update: (current: ScanDurabilityState<ScanPointCloudSnapshot>) => ScanDurabilityState<ScanPointCloudSnapshot>): void => {
    const next = update(scanDurabilityRef.current);
    scanDurabilityRef.current = next;
    setScanDurability(next);
  }, []);
  const scanSnapshot = viewportSnapshot(scanDurability);
  const scanAuthority = authoritativeSnapshot(scanDurability);
  const scanViewportProvenance = viewportProvenance(scanDurability);
  const [scanStoragePhase, setScanStoragePhase] = useState<ScanStoragePhase>(
    objectId === null || sessionId === null ? 'unbound' : 'loading',
  );
  const [scanStorageError, setScanStorageError] = useState<unknown | null>(null);
  const [scanPersistence, setScanPersistence] = useState<ScanPersistenceState | null>(null);
  const [localizationGeometry, setLocalizationGeometry] = useState<LocalizationSessionGeometry | null>(null);
  const [localizationRunListing, setLocalizationRunListing] = useState<LocalizationRunListing | null>(null);
  const [registrationArtifact, setRegistrationArtifact] = useState<RegistrationArtifact | null>(null);
  const [display, setDisplay] = useState<ScanDisplayOptions>(() => displayForScanMode('quality'));
  const [scanMode, setScanMode] = useState<ScanMode | null>('quality');
  const [viewCommand, setViewCommand] = useState<ViewCommand>({
    preset: 'perspective',
    sequence: 0,
  });
  const storageGeneration = useRef(0);
  const scanLoadSession = useRef<string | null>(null);
  const sessionEvidenceLoad = useRef<string | null>(null);
  /* Raw mirror of registrationArtifact. Builders and viewers never use it without the applicability predicate. */
  const registrationForSession = useRef<{ objectId: string; sessionId: string; artifact: RegistrationArtifact } | null>(null);
  const geometryGenerationRef = useRef<number | null>(null);
  const evidenceReady = useRef<Promise<unknown>>(Promise.resolve());
  const registrationStage = useRef<ScanRegistrationStage | null>(null);
  const provisionalTag = useRef<{ artifact: RegistrationArtifact | null; final: boolean }>({ artifact: null, final: false });
  const rejectedContinuation = useRef(false);
  const freshClearedRegistration = useRef(false);
  const continuationBaseRegistration = useRef<RegistrationArtifact | null>(null);
  const persistStageRef = useRef<(stage: ScanRegistrationStage) => void>(() => undefined);
  const [registrationStageView, setRegistrationStageView] = useState<RegistrationStageState>('idle');
  const [registrationNotice, setRegistrationNotice] = useState<{ translationM: number; yawDeg: number } | null>(null);
  const [scanIntentState, setScanIntentState] = useState<ScanStartIntent>('fresh');
  const applyGeometry = useCallback((geometry: LocalizationSessionGeometry | null): void => {
    geometryGenerationRef.current = geometryGeneration(geometry);
    setLocalizationGeometry(geometry);
  }, []);
  const publishRegistration = useCallback((owner: { objectId: string; sessionId: string } | null, artifact: RegistrationArtifact | null): void => {
    registrationForSession.current = owner === null || artifact === null ? null : { objectId: owner.objectId, sessionId: owner.sessionId, artifact };
    setRegistrationArtifact(owner === null ? null : artifact);
  }, []);
  const onRegistrationChange = useCallback((artifact: RegistrationArtifact | null): void => {
    publishRegistration(objectId === null || sessionId === null ? null : { objectId, sessionId }, artifact);
  }, [objectId, sessionId, publishRegistration]);
  useEffect(() => {
    registrationForSession.current = null;
    registrationStage.current = null;
    geometryGenerationRef.current = null;
    applyGeometry(null);
    setLocalizationRunListing(null);
    setRegistrationArtifact(null);
    setRegistrationStageView('idle');
    setRegistrationNotice(null);
  }, [objectId, sessionId, applyGeometry]);
  /* The registration to embed in a derivative of scanId, or null: same predicate as every other consumer. */
  const registrationEmbedFor = (scanId: string): ReturnType<typeof toOccupancyRegistration> => {
    const current = registrationForSession.current;
    if (current === null || current.objectId !== objectId || current.sessionId !== sessionId) return null;
    return toOccupancyRegistration(current.artifact, { geometryComputedAtMs: geometryGenerationRef.current, scanFrameId: scanId });
  };
  const scanIntent = useRef<ScanStartIntent>('fresh');
  const continuationBase = useRef<ScanPointCloudSnapshot | null>(null);
  const scanPersistenceController = useRef<ScanPersistenceController | null>(null);
  if (scanPersistenceController.current === null) {
    scanPersistenceController.current = new ScanPersistenceController({
      saveScan: saveObjectScanArtifact,
      saveOccupancy: saveSessionOccupancyArtifact,
      savePreview: saveSessionPreviewArtifact,
      fetchInfo: fetchScanInfo,
    }, (state) => {
      setScanPersistence(state);
      const candidate = activeScanCandidate.current;
      if (state === null) {
        activeScanCandidate.current = null;
        return;
      }
      if (candidate !== null && state.scanId === candidate.id) {
        if (state.scan === 'saving') updateScanDurability((current) => beginProvisionalScan(current, candidate.snapshot, candidate.id));
        else if (state.scan === 'verifying') updateScanDurability(markScanVerifying);
        else if (state.scan === 'unknown' || state.scan === 'derived_stale') updateScanDurability(markScanUnknown);
        else if (state.scan === 'saved') {
          setScanStoragePhase('saved');
          updateScanDurability((current) => setScanDerivativesReady(confirmScanCommitted(current, candidate.id), state.occupancy === 'ready'));
          /* Registration is a separate stage that starts once the scan is committed and never blocks it. */
          const stage = registrationStage.current;
          if (stage !== null && stage.scanId === candidate.id) persistStageRef.current(stage);
        } else if (state.scan === 'error' && state.outcome === 'confirmed_not_committed') {
          updateScanDurability((current) => confirmScanNotCommitted(current, state.previousKept));
        }
      } else if (state.scan === 'saved' && state.scanId !== null && scanDurabilityRef.current.durableId === state.scanId) {
        updateScanDurability((current) => setScanDerivativesReady(current, state.occupancy === 'ready'));
      }
    });
  }
  const persistenceController = scanPersistenceController.current;

  const waitForTagResult = (stage: ScanRegistrationStage): Promise<void> => {
    if (stage.tagFinal) return Promise.resolve();
    return new Promise<void>((resolve) => {
      let done = false;
      const timer = setTimeout(() => finish(), TAG_RESULT_WAIT_MS);
      const finish = (): void => {
        if (done) return;
        done = true;
        clearTimeout(timer);
        resolve();
      };
      stage.waiters.push(finish);
    });
  };

  /* Resolved exactly once per stage: occupancy, preview and the standalone write all use this one answer. */
  const resolveStageRegistration = (stage: ScanRegistrationStage): NonNullable<ScanRegistrationStage['resolution']> => {
    if (stage.resolution === null) {
      stage.resolution = (async () => {
        await waitForTagResult(stage);
        stage.windowClosed = true;
        const tag = stage.tagFinal ? stage.tagResult : null;
        if (stage.intent === 'fresh') {
          if (tag === null) return { artifact: null, conflict: null };
          const bound = bindRegistrationToScan(tag, stage.scanId);
          const binding = registrationBindingApplicability(bound, { geometryComputedAtMs: geometryGenerationRef.current, scanFrameId: stage.scanId });
          if (!binding.ok) {
            clientDiagnostics.warn('SCAN_REGISTRATION_NOT_APPLICABLE', { objectId: stage.objectId, sessionId: stage.sessionId, reason: binding.reason });
            return { artifact: null, conflict: null };
          }
          return { artifact: bound, conflict: null };
        }
        if (stage.extensionToBase === null) return { artifact: null, conflict: null };
        const decision = decideContinuationRegistration({
          base: stage.base, artifactB: tag, extensionToBase: stage.extensionToBase, mergedScanFrameId: stage.scanId,
        });
        if (decision.kind === 'none') return { artifact: null, conflict: null };
        return { artifact: decision.artifact, conflict: decision.kind === 'conflict' ? decision.agreement : null };
      })();
    }
    return stage.resolution;
  };

  const persistStageRegistration = (stage: ScanRegistrationStage): void => {
    if (stage.persistStarted) return;
    stage.persistStarted = true;
    const owns = (): boolean => registrationStage.current === stage;
    void (async () => {
      const resolved = await resolveStageRegistration(stage);
      if (!owns()) return;
      if (resolved.artifact === null) {
        if (stage.intent === 'fresh') {
          /* The replaced scan frame no longer has a registration: remove the old standalone file. */
          try { await deleteSessionRegistration(stage.objectId, stage.sessionId); }
          catch (reason) {
            clientDiagnostics.warn('SCAN_REGISTRATION_DELETE_FAILED', { objectId: stage.objectId, sessionId: stage.sessionId, error: reason instanceof Error ? reason.message : String(reason) });
          }
        }
        if (owns()) setRegistrationStageView('idle');
        return;
      }
      if (resolved.conflict !== null) {
        setRegistrationNotice({ translationM: resolved.conflict.translationDeltaM, yawDeg: resolved.conflict.yawDeltaDeg });
        clientDiagnostics.warn('SCAN_REGISTRATION_CONFLICT', { objectId: stage.objectId, translationM: resolved.conflict.translationDeltaM, yawDeg: resolved.conflict.yawDeltaDeg });
      }
      setRegistrationStageView('saving');
      try {
        const result = await saveSessionRegistration(stage.objectId, stage.sessionId, encodeRegistrationArtifact(resolved.artifact));
        if (!owns()) return;
        if (result?.ok !== true) throw new Error(result?.error ?? 'registration_rejected');
        publishRegistration({ objectId: stage.objectId, sessionId: stage.sessionId }, resolved.artifact);
        setRegistrationStageView('saved');
      } catch (reason) {
        if (!owns()) return;
        setRegistrationStageView('failed');
        clientDiagnostics.warn('SCAN_REGISTRATION_SAVE_FAILED', { objectId: stage.objectId, sessionId: stage.sessionId, error: reason instanceof Error ? reason.message : String(reason) });
      }
      void clientDiagnostics.flush();
    })();
  };
  persistStageRef.current = persistStageRegistration;

  const onTagRegistrationResult = useCallback((artifact: RegistrationArtifact | null, final: boolean): void => {
    provisionalTag.current = { artifact, final };
    if (!final) return;
    const stage = registrationStage.current;
    if (stage === null) {
      if (rejectedContinuation.current) {
        clientDiagnostics.info('SCAN_CONTINUATION_REGISTRATION_DISCARDED', {
          valid: artifact?.valid ?? null, reason: artifact?.reason ?? null, residualM: artifact?.residual_rms_m ?? null,
        });
      }
      return;
    }
    if (stage.windowClosed || stage.tagFinal) {
      clientDiagnostics.warn('SCAN_REGISTRATION_LATE_RESULT_DISCARDED', { objectId: stage.objectId, scanId: stage.scanId });
      return;
    }
    stage.tagResult = artifact;
    stage.tagFinal = true;
    for (const wake of stage.waiters.splice(0)) wake();
  }, []);

  const reloadSessionRegistration = useCallback((): void => {
    if (objectId === null || sessionId === null) return;
    const generation = storageGeneration.current;
    void loadSessionRegistration(objectId, sessionId).then((json) => {
      if (generation !== storageGeneration.current) return;
      publishRegistration({ objectId, sessionId }, json === null ? null : decodeRegistrationArtifact(json));
    }).catch((reason: unknown) => {
      clientDiagnostics.warn('SENSOR_REGISTRATION_RESTORE_FAILED', { objectId, sessionId, error: reason instanceof Error ? reason.message : String(reason) });
    });
  }, [objectId, sessionId, publishRegistration]);

  useEffect(() => {
    let cancelled = false;
    void deviceComputeProfilePromise.then((profile) => {
      if (cancelled) return;
      setComputeProfile(profile);
      clientDiagnostics.info('DEVICE_COMPUTE_PROFILE', {
        logicalProcessors: profile.reportedLogicalProcessors,
        reservedProcessors: profile.reservedLogicalProcessors,
        liveWorkerBudget: profile.liveWorkerBudget,
        postScanWorkerBudget: profile.postScanWorkerBudget,
        crossOriginIsolated: profile.crossOriginIsolated,
        sharedMemory: profile.sharedMemory,
        wasmThreads: profile.wasmThreads,
        webGpu: profile.webGpu,
        tier: profile.tier,
      });
      void clientDiagnostics.flush();
    });
    return () => { cancelled = true; };
  }, []);

  /*
   * Load the heavy renderer while the operator is still on the scanner page,
   * not at the exact moment Finish closes immersive AR. Its JS then sits in the
   * browser module cache; all scanner CSS is already resident in stable app.css.
   */
  useEffect(() => {
    void import('./components/ScanPointCloudViewport').catch((reason) => {
      clientDiagnostics.warn('SCAN_RESULT_RENDERER_PRELOAD_FAILED', {
        error: reason instanceof Error ? reason.message : String(reason),
      });
      void clientDiagnostics.flush();
    });
  }, []);

  const requestView = useCallback((preset: ViewPreset): void => {
    clientDiagnostics.info('SCAN_VIEW_PRESET', { preset });
    setViewCommand((current) => ({ preset, sequence: current.sequence + 1 }));
  }, []);

  const selectMode = useCallback((mode: ScanMode): void => {
    const next = scanViewForMode(mode);
    setScanMode(next.mode);
    setDisplay(next.display);
    clientDiagnostics.info('SCAN_RESULT_MODE', { mode, display: next.display });
  }, []);

  const toggleDisplay = useCallback((key: keyof ScanDisplayOptions): void => {
    setScanMode(null);
    setDisplay((current) => {
      const next = toggleScanLayer({ mode: null, display: current }, key).display;
      clientDiagnostics.info('SCAN_RESULT_LAYER_TOGGLE', { layer: key, enabled: next[key] });
      return next;
    });
  }, []);

  /* Restore the object-owned scan every time the scanner feature is opened. */
  useEffect(() => {
    storageGeneration.current += 1;
    persistenceController.reset();
    updateScanDurability(() => emptyScanDurability());
    scanLoadSession.current = null;
    sessionEvidenceLoad.current = null;
    scanIntent.current = 'fresh';
    continuationBase.current = null;
    continuationBaseRegistration.current = null;
    registrationStage.current = null;
    rejectedContinuation.current = false;
    provisionalTag.current = { artifact: null, final: false };
    setScanStorageError(null);
    applyGeometry(null);
    setLocalizationRunListing(null);
    publishRegistration(null, null);
    setRegistrationStageView('idle');
    setRegistrationNotice(null);

    const abandonScannerOwner = (): void => {
      storageGeneration.current += 1;
      persistenceController.abandon();
    };

    if (objectId === null || sessionId === null) {
      setScanStoragePhase('unbound');
      return abandonScannerOwner;
    }

    setScanStoragePhase('loading');
    return abandonScannerOwner;
  }, [objectId, sessionId, persistenceController, updateScanDurability, applyGeometry, publishRegistration]);

  useEffect(() => {
    if (objectId === null || sessionId === null || storageActionsDisabled) return undefined;
    const key = `${objectId}/${sessionId}`;
    const shouldLoadScan = scanLoadSession.current !== key;
    const shouldLoadEvidence = sessionEvidenceLoad.current !== key;
    if (!shouldLoadScan && !shouldLoadEvidence) return undefined;
    const generation = storageGeneration.current;
    let cancelled = false;
    if (shouldLoadEvidence) {
      evidenceReady.current = Promise.all([
        retryPreemptedRead(() => fetchLocalizationRuns(objectId, sessionId)).then(
          (listing) => ({ ok: true as const, listing }),
          (reason: unknown) => ({ ok: false as const, reason }),
        ),
        loadSessionRegistration(objectId, sessionId).then(
          (json) => ({ ok: true as const, json }),
          (reason: unknown) => ({ ok: false as const, reason }),
        ),
      ]).then(([geometryResult, registrationResult]) => {
        if (cancelled || generation !== storageGeneration.current) return;
        sessionEvidenceLoad.current = key;
        if (geometryResult.ok) {
          applyGeometry(geometryResult.listing.geometry);
          setLocalizationRunListing(geometryResult.listing);
        }
        else if (!isRequestCancellation(geometryResult.reason)) {
          const message = geometryResult.reason instanceof Error ? geometryResult.reason.message : String(geometryResult.reason);
          applyGeometry(null);
          setLocalizationRunListing(null);
          clientDiagnostics.warn('SENSOR_GEOMETRY_RESTORE_FAILED', { objectId, sessionId, error: message });
        }
        if (!registrationResult.ok) {
          const message = registrationResult.reason instanceof Error ? registrationResult.reason.message : String(registrationResult.reason);
          publishRegistration(null, null);
          clientDiagnostics.warn('SENSOR_REGISTRATION_RESTORE_FAILED', { objectId, sessionId, error: message });
        } else if (registrationResult.json === null) publishRegistration(null, null);
        else {
          /* Restored into the raw mirror too; every consumer still applies the predicate against the loaded scan. */
          try { publishRegistration({ objectId, sessionId }, decodeRegistrationArtifact(registrationResult.json)); }
          catch (reason) {
            const message = reason instanceof Error ? reason.message : String(reason);
            publishRegistration(null, null);
            clientDiagnostics.warn('SENSOR_REGISTRATION_RESTORE_FAILED', { objectId, sessionId, error: message });
          }
        }
        void clientDiagnostics.flush();
      });
    }
    if (!shouldLoadScan) return () => { cancelled = true; };
    void loadObjectScanArtifact(objectId, sessionId).then(
      (artifact) => {
        if (cancelled || generation !== storageGeneration.current) return;
        if (artifact === null) {
          scanLoadSession.current = key;
          setScanStoragePhase('none');
          clientDiagnostics.info('SCAN_RESULT_NOT_SAVED', { objectId, sessionId });
          void clientDiagnostics.flush();
          void persistenceController.reconcile(objectId, sessionId, null);
          return;
        }

        try {
          const decoded = decodeScanArtifactWithIdentity(artifact);
          const restored = decoded.snapshot;
          const loadedIdentity = decoded.identity;
          if (restored.pointCount <= 0) throw new Error('scan.error.no_stable_surface');
          const loadedScanId = loadedIdentity.frameId;
          const restoreSummary = Object.freeze({
            objectId,
            sessionId,
            bytes: loadedIdentity.byteLength,
            scanId: loadedIdentity.frameId,
            scanCrc32: loadedIdentity.crc32,
            stablePoints: restored.pointCount,
            tentativePoints: restored.tentativePointCount,
            manualPoints: restored.manualControlPoints.length,
            operatorEdges: restored.operatorEdges.length,
          });
          clientDiagnostics.info('SCAN_RESULT_RESTORE_VALIDATED', restoreSummary);
          void clientDiagnostics.flush();
          let decodedOccupancy: DecodedOccupancyArtifact | null = null;
          /* The restored registration is embedded only if it is bound to exactly this scan and geometry. */
          const buildOccupancy = async () => {
            await evidenceReady.current;
            const bytes = buildSessionOccupancyArtifact(restored, loadedIdentity, Date.now(), registrationEmbedFor(loadedScanId));
            decodedOccupancy = decodeOccupancySolverArtifact(bytes);
            return bytes;
          };
          const buildPreview = async (occupancyArtifact: Uint8Array | null) => {
            await evidenceReady.current;
            if (occupancyArtifact !== null && decodedOccupancy === null) {
              decodedOccupancy = decodeOccupancySolverArtifact(occupancyArtifact);
            }
            return buildVoxelPreviewArtifact(restored, loadedIdentity, {
              occupancyArtifact,
              decodedOccupancy,
              identity: loadedIdentity,
              registration: registrationEmbedFor(loadedScanId),
            });
          };
          void persistenceController.reconcile(objectId, sessionId, artifact, buildOccupancy, buildPreview, loadedScanId, loadedIdentity).then((reconciled) => {
            if (cancelled || generation !== storageGeneration.current) return;
            scanLoadSession.current = key;
            if (reconciled?.scan === 'saved' && reconciled.scanId === loadedScanId) {
              updateScanDurability((current) => reconcileDurableScan(current, restored, loadedScanId, reconciled.occupancy === 'ready'));
            } else {
              updateScanDurability((current) => markScanUnknown(beginProvisionalScan(emptyScanDurability(), restored, loadedScanId)));
            }
            selectMode('quality');
            setScanStoragePhase('loaded');
            clientDiagnostics.info('SCAN_RESULT_RESTORED', {
              objectId,
              sessionId,
              bytes: restoreSummary.bytes,
              stablePoints: restored.pointCount,
              tentativePoints: restored.tentativePointCount,
              manualPoints: restored.manualControlPoints.length,
              operatorEdges: restored.operatorEdges.length,
            });
            void clientDiagnostics.flush();
          });
        }
        catch (reason) {
          scanLoadSession.current = key;
          const message = reason instanceof Error ? reason.message : String(reason);
          setScanStorageError(TranslatedError.fromKey(message === 'scan.error.no_stable_surface'
            ? 'scan.error.no_stable_surface'
            : 'scan.error.restore_failed'));
          setScanStoragePhase('error');
          clientDiagnostics.warn('SCAN_RESULT_RESTORE_FAILED', { objectId, sessionId, error: message });
          void clientDiagnostics.flush();
        }
      },
      (reason) => {
        if (cancelled || generation !== storageGeneration.current) return;
        scanLoadSession.current = key;
        const message = reason instanceof Error ? reason.message : String(reason);
        setScanStorageError(reason);
        setScanStoragePhase('error');
        void persistenceController.reconcile(objectId, sessionId, null);
        clientDiagnostics.warn('SCAN_RESULT_RESTORE_FAILED', { objectId, sessionId, error: message });
        void clientDiagnostics.flush();
      },
    );

    return () => { cancelled = true; };
  }, [objectId, sessionId, persistenceController, selectMode, storageActionsDisabled, updateScanDurability, applyGeometry, publishRegistration]);

  const handleScanIntent = useCallback((intent: ScanStartIntent): boolean => {
    setScanStorageError(null);
    if (intent === 'continue') {
      const base = authoritativeSnapshot(scanDurability);
      if (!continueAvailable(scanDurability) || base === null || base.pointCount <= 0) {
        setScanStorageError(TranslatedError.fromKey('scan.error.continue_unavailable'));
        return false;
      }
      continuationBase.current = base;
      scanIntent.current = 'continue';
      setScanIntentState('continue');
      /* Keep the base registration only if it is applicable to the durable base scan right now. */
      const baseRegistration = registrationForSession.current;
      continuationBaseRegistration.current = baseRegistration !== null && baseRegistration.objectId === objectId && baseRegistration.sessionId === sessionId &&
        registrationApplicability(baseRegistration.artifact, { geometryComputedAtMs: geometryGenerationRef.current, scanFrameId: scanDurability.durableId }).ok
        ? baseRegistration.artifact
        : null;
      clientDiagnostics.info('SCAN_CONTINUATION_REQUEST', {
        objectId,
        stablePoints: base.pointCount,
        freeVoxels: base.knownFreeVoxelCount,
        unknownVoxels: base.unknownCandidateVoxelCount,
      });
      void clientDiagnostics.flush();
      return true;
    }

    scanIntent.current = 'fresh';
    setScanIntentState('fresh');
    continuationBase.current = null;
    /* MAX-405: the previous registration belongs to the frame being replaced; drop it from memory now. */
    continuationBaseRegistration.current = null;
    registrationStage.current = null;
    freshClearedRegistration.current = true;
    publishRegistration(null, null);
    setRegistrationNotice(null);
    setRegistrationStageView('idle');
    clientDiagnostics.info('SCAN_REPLACEMENT_REQUEST', {
      objectId,
      previousSavedScanKeptUntilCommit: savedScanAvailable(scanDurability),
    });
    void clientDiagnostics.flush();
    return true;
  }, [objectId, sessionId, scanDurability, publishRegistration]);

  const handleSessionActiveChange = useCallback((active: boolean): void => {
    setXrSessionActive(active);
    if (!active && scanDurabilityRef.current.captureActive) {
      const stateAtSessionEnd = scanDurabilityRef.current;
      updateScanDurability((current) => captureAborted(current));
      if (stateAtSessionEnd.phase === 'none') setScanStoragePhase(objectId === null ? 'unbound' : 'none');
      if (freshClearedRegistration.current) {
        /* Finish hands a scan over in the same tick; if none arrived the Fresh capture was aborted and the old registration still stands. */
        setTimeout(() => {
          if (!freshClearedRegistration.current) return;
          freshClearedRegistration.current = false;
          reloadSessionRegistration();
        }, 0);
      }
    }
  }, [objectId, updateScanDurability, reloadSessionRegistration]);

  const handleScanReady = useCallback((incoming: ScanPointCloudSnapshot | null): void => {
    setScanStorageError(null);

    if (incoming === null) {
      /*
       * The null callback marks XR startup. Keep the current durable scan and
       * its storage presentation intact until a replacement candidate exists.
       */
      const current = scanDurabilityRef.current;
      provisionalTag.current = { artifact: null, final: false };
      rejectedContinuation.current = false;
      registrationStage.current = null;
      updateScanDurability((state) => captureStarted(state, scanIntent.current));
      if (shouldShowScanningPresentation(current)) {
        setScanStoragePhase(objectId === null ? 'unbound' : 'scanning');
      }
      return;
    }

    let snapshot = incoming;
    freshClearedRegistration.current = false;
    const intentAtFinish = scanIntent.current;
    let extensionToBase: RigidTransform | null = null;
    if (intentAtFinish === 'continue') {
      const base = continuationBase.current;
      if (base === null) {
        setScanStorageError(TranslatedError.fromKey('scan.error.continue_base_missing'));
        setScanStoragePhase('error');
        clientDiagnostics.warn('SCAN_CONTINUATION_REJECTED', { objectId, reason: 'missing durable base' });
        void clientDiagnostics.flush();
        return;
      }

      const merged = registerAndMergeScanContinuation(base, incoming);
      if (!merged.ok) {
        setScanStoragePhase('loaded');
        setScanStorageError(TranslatedError.fromKey('scan.error.continue_not_saved', { reason: merged.reason }));
        clientDiagnostics.warn('SCAN_CONTINUATION_REJECTED', {
          objectId,
          reason: merged.reason,
          diagnostics: merged.diagnostics,
        });
        scanIntent.current = 'fresh';
        continuationBase.current = null;
        /* Rejected: nothing durable changes. The base scan and its registration stay; the tag result is only logged. */
        rejectedContinuation.current = true;
        continuationBaseRegistration.current = null;
        void clientDiagnostics.flush();
        return;
      }

      snapshot = merged.snapshot;
      extensionToBase = merged.extensionToBase;
      clientDiagnostics.info('SCAN_CONTINUATION_REGISTERED', {
        objectId,
        inliers: merged.diagnostics.inliers,
        overlapPercent: Math.round(merged.diagnostics.overlapRatio * 100),
        rmsMm: Math.round(merged.diagnostics.rmsMeters * 1000),
        yawDegrees: Number((merged.diagnostics.yawRadians * 180 / Math.PI).toFixed(1)),
        translation: merged.diagnostics.translation.map((value) => Number(value.toFixed(3))),
        mergedStablePoints: snapshot.pointCount,
        annotationsTruncated: merged.diagnostics.annotationsTruncated ?? false,
      });
      void clientDiagnostics.flush();
    }

    selectMode('quality');
    clientDiagnostics.info('SCAN_RESULT_READY', {
      stablePoints: snapshot.pointCount,
      tentativePoints: snapshot.tentativePointCount,
      manualPoints: snapshot.manualControlPoints.length,
      operatorEdges: snapshot.operatorEdges.length,
      freeVoxels: snapshot.knownFreeVoxelCount,
      unknownVoxels: snapshot.unknownCandidateVoxelCount,
      occupancyEnvelopeComponents: snapshot.occupancyEnvelopeComponentCount,
      quarantinedSurfaceComponents: snapshot.occupancyQuarantinedSurfaceComponentCount,
      quarantinedSurfacePoints: snapshot.occupancyQuarantinedSurfacePointCount,
      bridgeRescanComponents: snapshot.occupancyBridgeRescanComponentCount,
      bridgeEvidenceTruncated: snapshot.occupancyBridgeEvidenceTruncated,
    });
    void clientDiagnostics.flush();

    if (objectId === null) {
      updateScanDurability((current) => confirmScanNotCommitted(beginProvisionalScan(current, snapshot, 'unbound'), current.durableSnapshot !== null));
      setScanStorageError(TranslatedError.fromKey('scan.error.no_object_selected'));
      setScanStoragePhase('error');
      return;
    }

    let artifact: Uint8Array;
    let identity: ScanIdentity;
    try {
      const encoded = encodeScanArtifactWithIdentity(snapshot);
      artifact = encoded.bytes;
      identity = encoded.identity;
    }
    catch (reason) {
      const message = reason instanceof Error ? reason.message : String(reason);
      updateScanDurability((current) => confirmScanNotCommitted(beginProvisionalScan(current, snapshot, 'unencoded'), current.durableSnapshot !== null));
      setScanStorageError(TranslatedError.fromKey(message === SCAN_ANNOTATION_CAPACITY_ERROR
        ? SCAN_ANNOTATION_CAPACITY_ERROR
        : 'scan.error.prepare_failed'));
      setScanStoragePhase('error');
      clientDiagnostics.warn('SCAN_RESULT_SAVE_FAILED', { objectId, sessionId: scanSessionId, stage: 'encode', error: message });
      void clientDiagnostics.flush();
      return;
    }

    const generation = ++storageGeneration.current;
    const candidateId = identity.frameId;
    activeScanCandidate.current = { snapshot, id: candidateId };
    /* MAX-403/405/416: registration for this candidate is resolved after the bounded tag window, never before the upload. */
    const stage: ScanRegistrationStage | null = sessionId === null ? null : {
      objectId, sessionId, intent: intentAtFinish, scanId: candidateId,
      base: continuationBaseRegistration.current, extensionToBase,
      tagFinal: provisionalTag.current.final, tagResult: provisionalTag.current.final ? provisionalTag.current.artifact : null,
      windowClosed: false, waiters: [], resolution: null, persistStarted: false,
    };
    registrationStage.current = stage;
    continuationBaseRegistration.current = null;
    setRegistrationNotice(null);
    setRegistrationStageView(stage === null ? 'idle' : 'pending');
    const embedRegistration = async () => {
      if (stage === null) return null;
      const resolved = await resolveStageRegistration(stage);
      return resolved.artifact === null
        ? null
        : toOccupancyRegistration(resolved.artifact, { geometryComputedAtMs: geometryGenerationRef.current, scanFrameId: candidateId });
    };
    let decodedOccupancy: DecodedOccupancyArtifact | null = null;
    updateScanDurability((current) => beginProvisionalScan(current, snapshot, candidateId));
    setScanStoragePhase('saving');
    void persistenceController.save(
      objectId,
      scanSessionId,
      artifact,
      async () => {
        const bytes = buildSessionOccupancyArtifact(snapshot, identity, Date.now(), await embedRegistration());
        decodedOccupancy = decodeOccupancySolverArtifact(bytes);
        return bytes;
      },
      async (occupancyArtifact) => {
        if (occupancyArtifact !== null && decodedOccupancy === null) {
          decodedOccupancy = decodeOccupancySolverArtifact(occupancyArtifact);
        }
        return buildVoxelPreviewArtifact(snapshot, identity, {
          occupancyArtifact,
          decodedOccupancy,
          identity,
          registration: await embedRegistration(),
        });
      },
      storageActionsDisabled,
      candidateId,
      identity,
    ).then((state) => {
        if (generation !== storageGeneration.current) return;
        if (state?.scan !== 'saved') {
          const message = state?.scanError ?? SCAN_STORAGE_BLOCKED_MESSAGE;
          if (state?.outcome === 'unknown' || state?.scan === 'unknown' || state?.scan === 'derived_stale') {
            setScanStorageError(TranslatedError.fromKey('scan.storage.unknown'));
          } else if (state?.scan === 'blocked_recording') {
            setScanStorageError(TranslatedError.fromKey('scan.storage.paused'));
          } else if (state?.previousKept && previousKeptProvable(scanDurabilityRef.current)) {
            setScanStorageError(TranslatedError.fromKey('scan.storage.previous_kept', { reason: new Error(message) }));
          } else {
            setScanStorageError(TranslatedError.fromKey('scan.storage.not_committed', { reason: new Error(message) }));
          }
          setScanStoragePhase('error');
          if (state?.scan === 'error' && registrationStage.current === stage) {
            registrationStage.current = null;
            setRegistrationStageView('idle');
            if (intentAtFinish === 'fresh' && state?.previousKept) reloadSessionRegistration();
          }
          clientDiagnostics.warn('SCAN_RESULT_SAVE_FAILED', { objectId, sessionId: scanSessionId, stage: 'transport', error: message });
          void clientDiagnostics.flush();
          return;
        }
        setScanStoragePhase('saved');
        setScanStorageError(null);
        scanIntent.current = 'fresh';
        continuationBase.current = null;
        clientDiagnostics.info('SCAN_RESULT_SAVED', {
          objectId,
          sessionId: scanSessionId,
          bytes: identity.byteLength,
          stablePoints: snapshot.pointCount,
          manualPoints: snapshot.manualControlPoints.length,
          operatorEdges: snapshot.operatorEdges.length,
        });
        void clientDiagnostics.flush();
        if (state.occupancy === 'ready') {
          clientDiagnostics.info('SCAN_OCCUPANCY_SAVED', { objectId, sessionId: scanSessionId });
        } else if (state.occupancy === 'failed' || state.occupancy === 'unavailable' || state.occupancy === 'stale') {
          clientDiagnostics.warn('SCAN_OCCUPANCY_SAVE_FAILED', {
            objectId, sessionId: scanSessionId, error: state.occupancyError,
          });
          void clientDiagnostics.flush();
        }
        if (state.preview === 'ready') {
          clientDiagnostics.info('SCAN_VOXEL_PREVIEW_SAVED', { objectId, sessionId: scanSessionId });
        } else if (state.preview === 'failed' || state.preview === 'unavailable' || state.preview === 'stale') {
          clientDiagnostics.warn('SCAN_VOXEL_PREVIEW_SAVE_FAILED', {
            objectId, sessionId: scanSessionId, error: state.previewError,
          });
          void clientDiagnostics.flush();
        }
    });
  }, [objectId, sessionId, scanSessionId, persistenceController, selectMode, storageActionsDisabled, updateScanDurability, reloadSessionRegistration]);

  const retryPersistence = useCallback((): void => {
    if (storageActionsDisabled || objectId === null) return;
    const generation = storageGeneration.current;
    void persistenceController.retry(objectId, scanSessionId).then((state) => {
      if (generation !== storageGeneration.current || state === null) return;
      if (state.scan === 'saved') {
        setScanStoragePhase('saved');
        setScanStorageError(null);
        scanIntent.current = 'fresh';
        continuationBase.current = null;
      } else if (state.scan === 'blocked_recording') {
        setScanStorageError(state.scanError ?? SCAN_STORAGE_BLOCKED_MESSAGE);
      } else if (state.scan === 'error') {
        setScanStorageError(state.previousKept
          && previousKeptProvable(scanDurabilityRef.current)
          ? TranslatedError.fromKey('scan.storage.previous_kept', { reason: new Error(state.scanError ?? '') })
          : TranslatedError.fromKey('scan.storage.not_committed', { reason: new Error(state.scanError ?? '') }));
      } else if (state.scan === 'unknown' || state.scan === 'derived_stale') {
        setScanStorageError(TranslatedError.fromKey('scan.storage.unknown'));
      }
      const event = state.scan === 'blocked_recording' || state.scan === 'error' || state.scan === 'unknown' || state.scan === 'derived_stale'
        ? 'SCAN_RESULT_SAVE_FAILED'
        : state.occupancy === 'ready' ? 'SCAN_OCCUPANCY_SAVED'
          : state.preview === 'ready' ? 'SCAN_VOXEL_PREVIEW_SAVED'
              : state.preview === 'failed' || state.preview === 'unavailable' || state.preview === 'stale' ? 'SCAN_VOXEL_PREVIEW_SAVE_FAILED'
                : 'SCAN_OCCUPANCY_SAVE_FAILED';
      clientDiagnostics.info(event, { objectId, sessionId: scanSessionId, occupancyError: state.occupancyError, previewError: state.previewError });
      void clientDiagnostics.flush();
    });
  }, [objectId, scanSessionId, persistenceController, storageActionsDisabled]);

  const deleteSavedScan = useCallback((): void => {
    if (storageActionsDisabled) return;
    scanIntent.current = 'fresh';
    continuationBase.current = null;
    continuationBaseRegistration.current = null;
    registrationStage.current = null;
    publishRegistration(null, null);
    setRegistrationNotice(null);
    setRegistrationStageView('idle');
    persistenceController.reset();
    if (objectId === null) {
      setScanStoragePhase('unbound');
      return;
    }
    const generation = ++storageGeneration.current;
    setScanStoragePhase('deleting');
    setScanStorageError(null);
    void deleteObjectScanArtifact(objectId, scanSessionId).then(
      () => {
        if (generation !== storageGeneration.current) return;
        setScanStoragePhase('none');
        updateScanDurability(() => emptyScanDurability());
        clientDiagnostics.info('SCAN_RESULT_DELETED', { objectId, sessionId: scanSessionId });
        void clientDiagnostics.flush();
      },
      (reason) => {
        if (generation !== storageGeneration.current) return;
        const message = reason instanceof Error ? reason.message : String(reason);
        setScanStorageError(reason);
        setScanStoragePhase('error');
        clientDiagnostics.warn('SCAN_RESULT_DELETE_FAILED', { objectId, sessionId: scanSessionId, error: message });
        void clientDiagnostics.flush();
      },
    );
  }, [objectId, scanSessionId, persistenceController, storageActionsDisabled, updateScanDurability, publishRegistration]);

  const hasMeasuredScan = resultControlsAvailable(scanDurability) && scanSnapshot !== null && scanSnapshot.pointCount > 0;
  const durableViewport = scanViewportProvenance === 'durable' && scanAuthority !== null && scanAuthority === scanSnapshot;
  const registrationExpected = useMemo<RegistrationExpectation>(
    () => ({ geometryComputedAtMs: geometryGeneration(localizationGeometry), scanFrameId: durableViewport ? scanDurability.durableId : null }),
    [durableViewport, localizationGeometry, scanDurability.durableId],
  );
  const registrationFit = registrationArtifact === null ? null : registrationApplicability(registrationArtifact, registrationExpected);
  const viewerRegistration = durableViewport && registrationFit?.ok === true ? registrationArtifact : null;
  const sensorPlacementScene = useMemo(
    () => buildSensorPlacementScene(durableViewport ? localizationGeometry : null, durableViewport ? registrationArtifact : null, registrationExpected),
    [durableViewport, localizationGeometry, registrationArtifact, registrationExpected],
  );
  const registrationStatusText = registrationStageView === 'pending' ? t('scan.registration.state.pending')
    : registrationStageView === 'saving' ? t('scan.registration.state.saving')
      : registrationStageView === 'failed' ? t('scan.registration.state.failed')
        : registrationFit === null ? t('scan.registration.state.none')
          : registrationFit.ok ? t('scan.registration.state.saved')
            : t('scan.registration.state.stale', { reason: t(REGISTRATION_INAPPLICABLE_KEYS[registrationFit.reason]) });
  const httpsReady = xrCapabilities?.secureContext === true;
  const webXrReady = xrCapabilities?.immersiveAr === true;
  const extent = extentFromBounds(scanSnapshot?.bounds ?? null);
  const stableBands = scanSnapshot === null
    ? { weak: 0, improving: 0, stable: 0 }
    : countCoverageQualityBands(scanSnapshot.confidence);
  const tentativeBands = scanSnapshot === null
    ? { weak: 0, improving: 0, stable: 0 }
    : countCoverageQualityBands(scanSnapshot.tentativeConfidence);
  const qualityBands = {
    weak: stableBands.weak + tentativeBands.weak,
    improving: stableBands.improving + tentativeBands.improving,
    stable: stableBands.stable + tentativeBands.stable,
  };
  const storageGood = savedScanAvailable(scanDurability);
  const storageBusy = storageActionsDisabled || scanStoragePhase === 'loading' || scanStoragePhase === 'saving' || scanStoragePhase === 'deleting';
  const localizationDataReady = localizationReady(scanDurability) &&
    ((scanPersistence?.scan === 'saved' && scanPersistence.occupancy === 'ready') || previousKeptProvable(scanDurability));

  return (
    <main className="app-shell">
      {!xrSessionActive && (
        <header className="app-header app-header-compact">
          <div>
            <div className="eyebrow">{t('app.title')}</div>
            <h1>{t('scan.search_volume_title')}</h1>
          </div>
          <div className="header-badges">
            <span className={`badge ${httpsReady ? 'good' : 'warning'}`}>
              HTTPS {httpsReady ? '✓' : '—'}
            </span>
            <span className={`badge ${webXrReady ? 'good' : 'warning'}`}>
              WEBXR {webXrReady ? '✓' : '—'}
            </span>
            <span
              className={`badge ${computeProfile !== null && computeProfile.liveWorkerBudget > 0 ? 'good' : 'warning'}`}
              title={computeProfile === null ? t('scan.compute.negotiating') : computeProfileTitle(computeProfile, locale)}
            >
              {computeProfile === null
                ? 'CPU …'
                : `CPU ${computeProfile.reportedLogicalProcessors} · W${computeProfile.liveWorkerBudget}`}
            </span>
            <span className={`badge ${hasMeasuredScan ? 'good' : 'warning'}`}>
              {hasMeasuredScan ? t('scan.volume.ready') : t('scan.ready')}
            </span>
            <span className={`badge ${storageGood && localizationDataReady ? 'good' : storageGood || scanStoragePhase === 'error' ? 'warning' : ''}`}>
              {persistedScanStorageLabel(scanStoragePhase, objectId, scanPersistence, locale)}
            </span>
          </div>
        </header>
      )}

      {!xrSessionActive && scanStorageError !== null && (
        <div className="xr-runtime-error" role="alert">{formatErrorText(locale, scanStorageError)}</div>
      )}
      {storageActionsDisabled && (
        <div className="xr-runtime-warning" role="status">{t('scan.storage.paused')}</div>
      )}
      {!xrSessionActive && registrationNotice !== null && (
        <div className="xr-runtime-warning" role="status" data-registration-conflict="true">
          {t('scan.registration.conflict', {
            offset: formatMeters(registrationNotice.translationM, locale),
            rotation: `${formatNumber(locale, Math.round(registrationNotice.yawDeg * 10) / 10)}°`,
          })}
        </div>
      )}

      {!xrSessionActive && hasMeasuredScan && (
        <section className="toolbar toolbar-compact scan-toolbar" aria-label={t('scan.controls.label')}>
          <div className="scan-mode-controls" aria-label={t('scan.controls.modes')}>
            <button type="button" aria-pressed={scanMode === 'quality'} onClick={() => selectMode('quality')}>{t('scan.mode.quality')}</button>
            <button type="button" aria-pressed={scanMode === 'surface'} onClick={() => selectMode('surface')}>{t('scan.mode.surface')}</button>
            <button type="button" aria-pressed={scanMode === 'search-volume'} onClick={() => selectMode('search-volume')}>{t('scan.mode.search_volume')}</button>
            <button type="button" aria-pressed={scanMode === 'mesh'} onClick={() => selectMode('mesh')}>{t('scan.mode.mesh')}</button>
          </div>

          <div className="scan-quality-summary" aria-label={t('scan.controls.quality_bands')}>
            <span><i className="coverage-dot coverage-low" />{t('scan.quality.weak')} <strong>{formatNumber(locale, qualityBands.weak)}</strong></span>
            <span><i className="coverage-dot coverage-mid" />{t('scan.quality.improving')} <strong>{formatNumber(locale, qualityBands.improving)}</strong></span>
            <span><i className="coverage-dot coverage-high" />{t('scan.quality.stable')} <strong>{formatNumber(locale, qualityBands.stable)}</strong></span>
            <span>{t('scan.operator_pins')} <strong>{formatNumber(locale, scanSnapshot?.manualControlPoints.length ?? 0)}</strong></span>
            <span>{t('scan.operator_edges')} <strong>{formatNumber(locale, scanSnapshot?.operatorEdges.length ?? 0)}</strong></span>
          </div>

          <div className="scan-toolbar-group">
            <button type="button" onClick={() => requestView('perspective')}>3D</button>
            <button type="button" onClick={() => requestView('top')}>{t('scan.view.top')}</button>
            <button type="button" onClick={() => requestView('front')}>{t('scan.view.front')}</button>
          </div>
          <div className="scan-toolbar-group scan-layer-controls" aria-label={t('scan.controls.layers')}>
            <button type="button" aria-pressed={display.goodPoints} onClick={() => toggleDisplay('goodPoints')}>{t('scan.layer.stable')}</button>
            <button type="button" aria-pressed={display.badPoints} onClick={() => toggleDisplay('badPoints')}>{t('scan.layer.tentative')}</button>
            <button type="button" aria-pressed={display.mesh} onClick={() => toggleDisplay('mesh')}>{t('scan.layer.mesh')}</button>
            <button type="button" aria-pressed={display.freeAir} onClick={() => toggleDisplay('freeAir')}>{t('scan.layer.free')}</button>
            <button type="button" aria-pressed={display.unknownSpace} onClick={() => toggleDisplay('unknownSpace')}>{t('scan.layer.unknown')}</button>
            <button type="button" aria-pressed={display.manualPoints} onClick={() => toggleDisplay('manualPoints')}>{t('scan.layer.pins')}</button>
            <button type="button" aria-pressed={display.dimensions} onClick={() => toggleDisplay('dimensions')}>{t('scan.layer.dimensions')}</button>
          </div>
        </section>
      )}

      <XrRuntimePanel
        objectId={objectId}
        sessionId={sessionId}
        savedScanAvailable={storageGood && durableViewport && hasMeasuredScan}
        savedScanBusy={storageBusy}
        onScanIntent={handleScanIntent}
        onDeleteSavedScan={deleteSavedScan}
        onSessionActiveChange={handleSessionActiveChange}
        onPointCloudReady={handleScanReady}
        onCapabilitiesChange={setXrCapabilities}
        onRegistrationChange={onRegistrationChange}
        scanIntent={scanIntentState}
        baseScanFrameId={scanDurability.durableId}
        onTagRegistrationResult={onTagRegistrationResult}
      />

      {!xrSessionActive && hasMeasuredScan && scanSnapshot !== null && (
        <section className="workspace" data-scan-provenance={scanViewportProvenance}>
          {scanViewportProvenance === 'provisional' && (
            <div className="xr-runtime-warning" role="status" data-scan-provenance="provisional">
              {t('scan.storage.provisional')}
            </div>
          )}
          <ResultViewportErrorBoundary key={`${objectId ?? 'none'}-${scanSnapshot.depthFrameCount}-${scanSnapshot.pointCount}`} formatFailure={(error) => t('scan.renderer.failed', { error })}>
            <Suspense
              fallback={
                <div className="viewport-shell scan-viewport" role="status" aria-live="polite">
                  {t('scan.loading_renderer')}
                </div>
              }
            >
              <ScanPointCloudViewport
                snapshot={scanSnapshot}
                viewCommand={viewCommand}
                display={display}
                mode={scanMode}
                sensorMarkers={durableViewport ? sensorPlacementScene.markers : []}
                sensorGeometry={durableViewport ? localizationGeometry : null}
                registrationArtifact={viewerRegistration}
                currentScanId={durableViewport ? scanDurability.durableId : null}
                objectId={objectId}
                sessionId={sessionId}
                localizationRuns={durableViewport && localizationRunListing?.object_id === objectId && localizationRunListing.session_id === sessionId
                  ? localizationRunListing.runs
                  : []}
              />
            </Suspense>
          </ResultViewportErrorBoundary>

          <aside className="side-panel scan-result-panel">
            <section className="panel-card">
              <div className="panel-title">{t('scan.search_volume_title')}</div>
              <dl className="status-list scan-result-primary">
                <div><dt>{t('scan.volume.sd_persistence')}</dt><dd>{persistedScanStorageLabel(scanStoragePhase, objectId, scanPersistence, locale)}</dd></div>
                <div><dt>{t('scan.volume.occupancy')}</dt><dd>{occupancyStatusLabel(scanPersistence, locale)}</dd></div>
                <div><dt>{t('scan.volume.preview')}</dt><dd>{previewStatusLabel(scanPersistence, locale)}</dd></div>
                <div><dt>{t('scan.volume.registration')}</dt><dd>{registrationStatusText}</dd></div>
                <div><dt>{t('scan.volume.stable_points')}</dt><dd>{formatNumber(locale, scanSnapshot.pointCount)}</dd></div>
                <div><dt>{t('scan.volume.tentative_points')}</dt><dd>{formatNumber(locale, scanSnapshot.tentativePointCount)}</dd></div>
                <div><dt>{t('scan.volume.manual_pins')}</dt><dd>{formatNumber(locale, scanSnapshot.manualControlPoints.length)}</dd></div>
                <div><dt>{t('scan.volume.confirmed_edges')}</dt><dd>{formatNumber(locale, scanSnapshot.operatorEdges.length)}</dd></div>
                <div><dt>{t('scan.volume.free_voxels')}</dt><dd>{formatNumber(locale, scanSnapshot.knownFreeVoxelCount)}</dd></div>
                <div><dt>{t('scan.volume.surface_voxels')}</dt><dd>{formatNumber(locale, scanSnapshot.occupiedSurfaceVoxelCount)}</dd></div>
                <div><dt>{t('scan.volume.unknown_voxels')}</dt><dd>{formatNumber(locale, scanSnapshot.unknownCandidateVoxelCount)}</dd></div>
                {scanSnapshot.occupancyCellsSaturated === true && (
                  <div><dt>{t('scan.volume.occupancy_full')}</dt><dd>{t('scan.volume.updates_dropped', { count: formatNumber(locale, scanSnapshot.occupancyDroppedCellUpdates ?? 0) })}</dd></div>
                )}
                {scanSnapshot.occupancyBridgeRescanComponentCount > 0 && (
                  <div><dt>{t('scan.volume.rescan_islands')}</dt><dd>{formatNumber(locale, scanSnapshot.occupancyBridgeRescanComponentCount)}</dd></div>
                )}
                <div><dt>{t('scan.volume.residual')}</dt><dd>{formatNumber(locale, Math.round(scanSnapshot.averageStableResidualMeters * 1000))} mm</dd></div>
                {extent !== null && (
                  <>
                    <div><dt>{t('scan.volume.extent_x')}</dt><dd>{formatMeters(extent[0], locale)}</dd></div>
                    <div><dt>{t('scan.volume.extent_y')}</dt><dd>{formatMeters(extent[1], locale)}</dd></div>
                    <div><dt>{t('scan.volume.height_z')}</dt><dd>{formatMeters(extent[2], locale)}</dd></div>
                  </>
                )}
              </dl>

              {scanPersistence?.scan === 'saved' &&
                (scanPersistence.occupancy === 'failed' || scanPersistence.occupancy === 'unavailable' || scanPersistence.occupancy === 'stale') && (
                <p className="scan-volume-note" role="status">
                  {scanPersistence.occupancy === 'stale'
                    ? t('scan.occupancy.stale')
                    : t('scan.volume.occupancy_not_ready')}
                </p>
              )}

              {scanPersistence?.scan === 'saved' &&
                (scanPersistence.preview === 'failed' || scanPersistence.preview === 'unavailable' || scanPersistence.preview === 'stale') && (
                <p className="scan-volume-note" role="status">
                  {scanPersistence.preview === 'stale'
                    ? t('scan.preview.stale')
                    : t('scan.volume.preview_not_ready')}
                </p>
              )}

              {persistenceController.retryAvailable && (
                <button type="button" className="scan-clear-button" disabled={storageActionsDisabled || scanPersistence?.scan === 'saving' || scanPersistence?.scan === 'verifying' || scanPersistence?.occupancy === 'saving' || scanPersistence?.preview === 'saving'} onClick={retryPersistence}>
                  {scanPersistence?.scan === 'blocked_recording' || scanPersistence?.scan === 'error' || scanPersistence?.scan === 'unknown' ? t('scan.retry.scan') : scanPersistence?.occupancy === 'failed' || scanPersistence?.occupancy === 'stale' ? t('scan.retry.occupancy') : t('scan.retry.preview')}
                </button>
              )}

              {scanSnapshot.occupancyBridgeRescanComponentCount > 0 && (
                <p className="scan-volume-note">
                  {t('scan.volume.bridge_note', { count: formatNumber(locale, scanSnapshot.occupancyBridgeRescanComponentCount) })}
                </p>
              )}

              <p className="scan-volume-note">
                {t('scan.volume.legend')}
              </p>

              <details className="xr-technical-details scan-result-details">
                <summary>{t('scan.details.technical')}</summary>
                <dl className="status-list">
                  <div><dt>{t('scan.details.retained')}</dt><dd>{formatNumber(locale, scanSnapshot.rawPointCount)}</dd></div>
                  <div><dt>{t('scan.details.promoted')}</dt><dd>{formatNumber(locale, scanSnapshot.promotedPointCount)}</dd></div>
                  <div><dt>{t('scan.details.unknown_components')}</dt><dd>{formatNumber(locale, scanSnapshot.occupancyEnvelopeComponentCount)}</dd></div>
                  <div><dt>{t('scan.details.quarantined_components')}</dt><dd>{formatNumber(locale, scanSnapshot.occupancyQuarantinedSurfaceComponentCount)}</dd></div>
                  <div><dt>{t('scan.details.quarantined_points')}</dt><dd>{formatNumber(locale, scanSnapshot.occupancyQuarantinedSurfacePointCount)}</dd></div>
                  <div><dt>{t('scan.details.weak_bridge')}</dt><dd>{formatNumber(locale, scanSnapshot.occupancyBridgeRescanComponentCount)}</dd></div>
                  <div><dt>{t('scan.details.bridge_diagnostic')}</dt><dd>{scanSnapshot.occupancyBridgeEvidenceTruncated ? t('scan.details.bounded_incomplete') : t('scan.details.complete')}</dd></div>
                  <div><dt>{t('scan.details.ready_cells')}</dt><dd>{formatNumber(locale, scanSnapshot.readyCoverageCellCount)}</dd></div>
                  <div><dt>{t('scan.details.keyframes')}</dt><dd>{formatNumber(locale, scanSnapshot.integrationKeyframes)}</dd></div>
                  <div><dt>{t('scan.details.coverage_cells')}</dt><dd>{formatNumber(locale, scanSnapshot.coverageCellCount)}</dd></div>
                  <div><dt>{t('scan.details.coverage_cell')}</dt><dd>{formatNumber(locale, Math.round(scanSnapshot.coverageCellSizeMeters * 1000))} mm</dd></div>
                  <div><dt>{t('scan.details.surface_voxel')}</dt><dd>{formatNumber(locale, Math.round(scanSnapshot.voxelSizeMeters * 1000))} mm</dd></div>
                  <div><dt>{t('scan.details.occupancy_voxel')}</dt><dd>{formatNumber(locale, Math.round(scanSnapshot.occupancyVoxelSizeMeters * 1000))} mm</dd></div>
                  <div><dt>{t('scan.details.adaptive_reps')}</dt><dd>{formatNumber(locale, scanSnapshot.adaptivePointsPerCell)}</dd></div>
                  <div><dt>{t('scan.details.polish_frames')}</dt><dd>{formatNumber(locale, scanSnapshot.polishFrames)}</dd></div>
                  <div><dt>{t('scan.details.residual_rejects')}</dt><dd>{formatNumber(locale, scanSnapshot.residualRejectedSamples)}</dd></div>
                  <div><dt>{t('scan.details.stale_pruned')}</dt><dd>{formatNumber(locale, scanSnapshot.staleTentativePruned)}</dd></div>
                  <div><dt>{t('scan.details.candidate_samples')}</dt><dd>{formatNumber(locale, scanSnapshot.candidateSampleCount)}</dd></div>
                  <div><dt>{t('scan.details.decimated_rejected')}</dt><dd>{formatNumber(locale, scanSnapshot.discardedSampleCount)}</dd></div>
                  <div><dt>{t('scan.details.replacements')}</dt><dd>{formatNumber(locale, scanSnapshot.replacementCount)}</dd></div>
                  <div><dt>{t('scan.details.memory_budget')}</dt><dd>{formatNumber(locale, scanSnapshot.memoryBudgetPoints)}</dd></div>
                  <div><dt>{t('scan.details.coverage_rejects')}</dt><dd>{formatNumber(locale, scanSnapshot.coverageRejectedCount)}</dd></div>
                  <div><dt>{t('scan.details.depth_frames')}</dt><dd>{formatNumber(locale, scanSnapshot.depthFrameCount)}</dd></div>
                  <div><dt>{t('scan.details.integrated_frames')}</dt><dd>{formatNumber(locale, scanSnapshot.sampledDepthFrames)}</dd></div>
                  <div><dt>{t('scan.details.depth_cap')}</dt><dd>{formatMeters(scanSnapshot.maxDepthMeters, locale)}</dd></div>
                </dl>
              </details>

              <button type="button" className="scan-clear-button" disabled={storageBusy} onClick={deleteSavedScan}>
                {scanStoragePhase === 'deleting' ? t('scan.deleting') : t('scan.delete_saved')}
              </button>
            </section>
          </aside>
        </section>
      )}
    </main>
  );
}
