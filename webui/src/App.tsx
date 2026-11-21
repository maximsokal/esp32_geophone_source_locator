import { Suspense, useCallback, useEffect, useMemo, useRef, useState } from 'react';

import './scan.css';

import {
  deleteObjectScanArtifact,
  fetchLocalizationRuns,
  fetchScanInfo,
  loadSessionTagPositions,
  isRequestCancellation,
  deleteSessionRegistration,
  deleteSessionTagPositions,
  loadObjectScanArtifact,
  loadSessionRegistration,
  saveObjectScanArtifact,
  saveSessionRegistration,
  saveSessionTagPositions,
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
import { ScanPersistenceController, SCAN_STORAGE_BLOCKED_MESSAGE, scanPersistenceSessionId, type ScanPersistenceState } from './xr/scanPersistence';
import { scannerStorageActionsDisabled } from './features/objects/measurementTransition';
import type { ViewCommand, ViewPreset } from './components/SceneViewport';
import { XrRuntimePanel, type ScanStartIntent } from './components/XrRuntimePanel';
import {
  resultViewportIdentityKey,
  VIEWPORT_FAILURE_MAX_ATTEMPTS,
  ViewportFailureBoundary,
} from './components/ViewportFailureBoundary';
import { clientDiagnostics } from './data/clientDiagnostics';
import { countCoverageQualityBands } from './xr/coverageQuality';
import {
  deviceComputeProfilePromise,
  type DeviceComputeProfile,
} from './xr/deviceComputeProfile';
import {
  SCAN_ANNOTATION_CAPACITY_ERROR,
} from './xr/scanArtifact';
import { buildSensorPlacementScene, mergeLiveTagAnchorMarkers } from './xr/sensorPlacementScene';
import type { TagSensorAnchor } from './xr/fiducial/liveTagTracker';
import { buildTagLayout, tagAnchorsToBaseFrame, tagLayoutQuality, tagLayoutReplaces } from './xr/fiducial/tagLayout';
import { encodeTagPositions, tagPositionsFromLayout, decodeTagPositions, type TagPositionsArtifact } from './xr/fiducial/tagPositionsArtifact';
import { computeScanDistanceRefinement } from './xr/scanDistanceRefinement';
import { ScanDistanceRefinementCard } from './components/ScanDistanceRefinementCard';
import {
  CONTINUATION_TRANSIENT_BUDGET_BYTES,
  type ScanContinuationPhase,
  type RigidTransform,
} from './xr/scanContinuation';
import { ContinuationBudgetExceededError, ContinuationCancelledError, ContinuationClient } from './xr/continuationClient';
import {
  ScanEncodeClient,
  ScanEncodeCancelledError,
  SCAN_ENCODE_TRANSIENT_COPY_BUDGET_BYTES,
  releaseScanEncodeResult,
  type ScanEncodeResult,
} from './xr/scanEncodeClient';
import {
  ScanDecodeCancelledError,
  ScanDecodeClient,
  SCAN_DECODE_TRANSIENT_BUDGET_BYTES,
  releaseScanDecodeResult,
} from './xr/scanDecodeClient';
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
/*
 * Imported statically, not lazily. Board 2026-11-17: the viewer chunk was fetched with import() while the scan was still being
 * saved to the board, whose HTTPS server takes one client at a time; the fetch failed ("Failed to fetch dynamically imported
 * module"), and a failed import() is remembered by the browser for the page's life, so Retry could not help and the 3D result
 * was never shown. In the scanner's own chunk the viewer arrives with the scanner, before any scan exists.
 */
import { ScanPointCloudViewport } from './components/ScanPointCloudViewport';


type ScanStoragePhase =
  | 'unbound'
  | 'loading'
  | 'reading'
  | 'none'
  | 'scanning'
  | 'continuing'
  | 'preparing'
  | 'saving'
  | 'saved'
  | 'loaded'
  | 'deleting'
  | 'error';

interface AppProps {
  objectId: string | null;
  /* Existing session scans remain session-owned; null marks an explicit
   * workflow scan that is saved to pre-record object staging. */
  sessionId: string | null;
  status: DeviceStatus | null;
  onScanSaved?: () => void | Promise<void>;
}

/** Build the React identity for the result viewport from the durable scan ID or provisional generation. */
export function resultViewportKey(
  scan: { objectId: string | null; durableScanId: string | null },
  provisionalGeneration: number,
): string {
  return `${scan.objectId ?? 'none'}:${resultViewportIdentityKey(scan.durableScanId, provisionalGeneration)}`;
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
    loading: 'scan.storage.loading', reading: 'scan.storage.reading', none: 'scan.storage.none', scanning: 'scan.storage.scanning', continuing: 'scan.storage.continuing',
    preparing: 'scan.storage.preparing', saving: 'scan.storage.saving', saved: 'scan.storage.saved', loaded: 'scan.storage.loaded',
    deleting: 'scan.storage.deleting', error: 'scan.storage.error',
  } as const;
  return translate(locale, keys[phase], {
    object: objectId ?? translate(locale, 'scan.storage.no_object'),
  });
}

/*
 * The measurement workflow leaves the scanner only once the solver input is on the device: the occupancy (the scan's
 * FREE and OCCUPIED voxels) is saved, or it cannot exist for this scan (unavailable). A transport failure -- Wi-Fi
 * dropping during that upload -- keeps the operator here with the retry; leaving used to record the session without its
 * voxels, so the source calculation silently ran unconstrained.
 */
function workflowScanSettled(persistence: ScanPersistenceState): boolean {
  return persistence.scan === 'saved' && (persistence.occupancy === 'ready' || persistence.occupancy === 'unavailable');
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
  if (phase === 'reading') return scanStorageLabel(phase, objectId, locale);
  if (phase === 'preparing') return scanStorageLabel(phase, objectId, locale);
  if (persistence?.scan === 'verifying') return translate(locale, 'scan.storage.verifying');
  if (persistence?.scan === 'unknown' || persistence?.scan === 'derived_stale') return translate(locale, 'scan.storage.unknown');
  const scanLabel = scanStorageLabel(phase, objectId, locale);
  if (phase !== 'saved' && phase !== 'loaded') return scanLabel;
  return `${scanLabel} | ${occupancyStatusLabel(persistence, locale)}`;
}

const TAG_UPLOAD_MAX_RETRIES = 3;
const TAG_UPLOAD_RETRY_BASE_MS = 5000;

export default function App({ objectId, sessionId, status, onScanSaved }: AppProps) {
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
  const [liveTagAnchors, setLiveTagAnchors] = useState<readonly TagSensorAnchor[]>([]);
  /* True once the scanner has handed over its tags for this scan (an empty list counts: it means none were seen). */
  const [tagAnchorsDelivered, setTagAnchorsDelivered] = useState(false);
  const tagUploadKeyRef = useRef<string | null>(null);
  /* A failed save/delete of the tag sidecar is retried a few times with growing delays (issue #20); the tick re-runs the upload effect. */
  const [tagRetryTick, setTagRetryTick] = useState(0);
  const tagRetryAttempts = useRef(0);
  const tagRetryTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  /*
   * Who owns the delivered anchors. `baseId` is the durable scan that was on screen when the scanner started: anchors may only be saved once a
   * DIFFERENT durable scan exists (a rejected continuation leaves the base durable, so nothing is written against it). `frame` is 'pending'
   * while a continuation is being merged, the accepted T_BA once it was (anchors are in frame B and must be moved into A), or 'fresh'.
   */
  const [tagOwner, setTagOwner] = useState<{ baseId: string | null; frame: 'fresh' | 'pending' | RigidTransform }>({ baseId: null, frame: 'fresh' });
  /* The best layout saved for the scan on screen: a weaker look at the same scan (rejected continuation) must not replace it. */
  const savedTagLayoutRef = useRef<{ scanId: string; nodes: number; confirmed: number } | null>(null);
  /* The tag layout read back from the ESP32 (or last saved) for this object/session: the baseline a continuation inherits, and what survives a page reload. */
  const savedTagArtifactRef = useRef<{ objectId: string; sessionId: string; artifact: TagPositionsArtifact } | null>(null);
  /* Anchors belong to the scan just made for this target; another object/session starts without them. */
  useEffect(() => { setLiveTagAnchors([]); }, [objectId, sessionId]);
  const [continuationPhase, setContinuationPhase] = useState<ScanContinuationPhase | null>(null);
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
  const scanDecodeOwnerGeneration = useRef(0);
  const scanDecodeClient = useRef<ScanDecodeClient | null>(null);
  const scanDecodePreviousPhase = useRef<ScanStoragePhase | null>(null);
  const scanEncodeOwner = useRef({ objectId, sessionId });
  scanEncodeOwner.current = { objectId, sessionId };
  const scanEncodeClient = useRef<ScanEncodeClient | null>(null);
  const scanEncodeOwnerGeneration = useRef(0);
  const scanEncodePreviousPhase = useRef<ScanStoragePhase | null>(null);
  const scanLoadSession = useRef<string | null>(null);
  const sessionEvidenceLoad = useRef<string | null>(null);
  /* A failed READ of the saved scan (HTTP/SD hiccup, decode worker crash) is retryable in place; a scan with no usable surface is not. */
  const [scanReadRetryable, setScanReadRetryable] = useState(false);
  const [scanLoadAttempt, setScanLoadAttempt] = useState(0);
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
  const continuationClient = useRef<ContinuationClient | null>(null);
  const continuationOwnerGeneration = useRef(0);
  const continuationPreviousPhase = useRef<ScanStoragePhase | null>(null);
  const continuationActive = useRef(false);
  const cancelScanEncode = useCallback((reason: string): void => {
    scanEncodeOwnerGeneration.current += 1;
    const client = scanEncodeClient.current;
    const wasBusy = client?.busy ?? false;
    client?.cancel(reason);
    if (wasBusy) {
      setScanStoragePhase(scanEncodePreviousPhase.current ?? 'loaded');
      scanEncodePreviousPhase.current = null;
    }
  }, []);
  const cancelScanDecode = useCallback((reason: string): void => {
    scanDecodeOwnerGeneration.current += 1;
    const client = scanDecodeClient.current;
    const wasBusy = client?.busy ?? false;
    client?.cancel(reason);
    if (wasBusy) {
      setScanStoragePhase(scanDecodePreviousPhase.current ?? 'loading');
      scanDecodePreviousPhase.current = null;
    }
  }, []);
  const cancelContinuation = useCallback((reason: string, notify = false): void => {
    continuationOwnerGeneration.current += 1;
    const active = continuationActive.current;
    continuationClient.current?.cancel(reason);
    if (!active) return;
    continuationActive.current = false;
    setScanStoragePhase(continuationPreviousPhase.current ?? 'loaded');
    continuationPreviousPhase.current = null;
    setContinuationPhase(null);
    if (notify) setScanStorageError(TranslatedError.fromKey('scan.continuation.cancelled'));
  }, []);
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

  useEffect(() => () => {
    scanDecodeClient.current?.dispose();
    scanDecodeClient.current = null;
    scanDecodePreviousPhase.current = null;
    scanEncodeOwnerGeneration.current += 1;
    scanEncodeClient.current?.dispose();
    scanEncodeClient.current = null;
    scanEncodePreviousPhase.current = null;
    continuationOwnerGeneration.current += 1;
    continuationClient.current?.dispose();
    continuationClient.current = null;
    continuationActive.current = false;
  }, []);

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
    cancelScanEncode('Scanner owner changed.');
    cancelScanDecode('Scanner owner changed.');
    storageGeneration.current += 1;
    persistenceController.reset();
    updateScanDurability(() => emptyScanDurability());
    setContinuationPhase(null);
    scanLoadSession.current = null;
    sessionEvidenceLoad.current = null;
    savedTagArtifactRef.current = null;
    savedTagLayoutRef.current = null;
    setScanReadRetryable(false);
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
      cancelScanEncode('Scanner owner changed.');
      cancelScanDecode('Scanner owner changed.');
      continuationOwnerGeneration.current += 1;
      continuationClient.current?.cancel('Scanner owner changed.');
      continuationActive.current = false;
      continuationPreviousPhase.current = null;
      persistenceController.abandon();
    };

    if (objectId === null || sessionId === null) {
      setScanStoragePhase('unbound');
      return abandonScannerOwner;
    }

    setScanStoragePhase('loading');
    return abandonScannerOwner;
  }, [objectId, sessionId, persistenceController, updateScanDurability, applyGeometry, publishRegistration, cancelContinuation, cancelScanEncode, cancelScanDecode]);

  useEffect(() => {
    if (objectId === null || sessionId === null || storageActionsDisabled) return undefined;
    const key = `${objectId}/${sessionId}`;
    const shouldLoadScan = scanLoadSession.current !== key;
    const shouldLoadEvidence = sessionEvidenceLoad.current !== key;
    if (!shouldLoadScan && !shouldLoadEvidence) return undefined;
    const generation = storageGeneration.current;
    const decodeOwnerGeneration = scanDecodeOwnerGeneration.current;
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
        loadSessionTagPositions(objectId, sessionId).then(
          (json) => ({ ok: true as const, json }),
          (reason: unknown) => ({ ok: false as const, reason }),
        ),
      ]).then(([geometryResult, registrationResult, tagsResult]) => {
        if (cancelled || generation !== storageGeneration.current) return;
        /* The saved tag layout is the baseline: after a reload a weaker look must still not replace it, and a continuation inherits it (issue #38). */
        if (tagsResult.ok && typeof tagsResult.json === 'string') {
          try {
            const artifact = decodeTagPositions(tagsResult.json);
            savedTagArtifactRef.current = { objectId, sessionId, artifact };
            savedTagLayoutRef.current = { scanId: artifact.scan_frame_id ?? '', nodes: artifact.nodes.length, confirmed: artifact.nodes.filter((node) => node.confirmed).length };
          } catch (reason) {
            clientDiagnostics.warn('TAG_POSITIONS_RESTORE_FAILED', { objectId, sessionId, error: reason instanceof Error ? reason.message : String(reason) });
          }
        }
        /* A failed sub-request stays retryable (issue #22): only a fully loaded evidence set is remembered for this screen. */
        if (geometryResult.ok && registrationResult.ok) sessionEvidenceLoad.current = key;
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
      async (artifact) => {
        if (cancelled || generation !== storageGeneration.current || decodeOwnerGeneration !== scanDecodeOwnerGeneration.current) return;
        if (artifact === null) {
          scanLoadSession.current = key;
          setScanStoragePhase('none');
          clientDiagnostics.info('SCAN_RESULT_NOT_SAVED', { objectId, sessionId });
          void clientDiagnostics.flush();
          void persistenceController.reconcile(objectId, sessionId, null);
          return;
        }

        setScanStoragePhase('reading');
        scanDecodePreviousPhase.current = 'loading';
        const decoder = scanDecodeClient.current ?? (scanDecodeClient.current = new ScanDecodeClient());
        let decodedResult: Awaited<ReturnType<ScanDecodeClient['decode']>> | null = null;
        try {
          decodedResult = await decoder.decode(artifact, {
            generation: decodeOwnerGeneration,
            transferInput: true,
            budgetLimitBytes: SCAN_DECODE_TRANSIENT_BUDGET_BYTES,
            onFallback: () => clientDiagnostics.warn('SCAN_RESULT_RESTORE_WORKER_UNAVAILABLE', {
              objectId, sessionId, reason: 'Worker is unavailable',
            }),
          });
          if (cancelled || generation !== storageGeneration.current || decodeOwnerGeneration !== scanDecodeOwnerGeneration.current) {
            releaseScanDecodeResult(decodedResult);
            return;
          }
          scanDecodePreviousPhase.current = null;
          const restored = decodedResult.snapshot;
          const loadedIdentity = decodedResult.identity;
          if (restored.pointCount <= 0) {
            releaseScanDecodeResult(decodedResult);
            decodedResult = null;
            throw new Error('scan.error.no_stable_surface');
          }
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
            decodeCounters: decodedResult.counters,
          });
          clientDiagnostics.info('SCAN_RESULT_RESTORE_VALIDATED', restoreSummary);
          /* Defer the best-effort flush until reconciliation commits; diagnostics never gate restore authority. */
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
          void persistenceController.reconcile(objectId, sessionId, null, buildOccupancy, buildPreview, loadedScanId, loadedIdentity).then((reconciled) => {
            if (cancelled || generation !== storageGeneration.current || decodeOwnerGeneration !== scanDecodeOwnerGeneration.current) return;
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
          decodedResult = null;
        }
        catch (reason) {
          if (decodedResult !== null) releaseScanDecodeResult(decodedResult);
          scanDecodePreviousPhase.current = null;
          if (cancelled || generation !== storageGeneration.current || decodeOwnerGeneration !== scanDecodeOwnerGeneration.current ||
              reason instanceof ScanDecodeCancelledError) return;
          scanLoadSession.current = key;
          const message = reason instanceof Error ? reason.message : String(reason);
          setScanReadRetryable(message !== 'scan.error.no_stable_surface');
          setScanStorageError(TranslatedError.fromKey(message === 'scan.error.no_stable_surface'
            ? 'scan.error.no_stable_surface'
            : 'scan.error.restore_failed'));
          setScanStoragePhase('error');
          clientDiagnostics.warn('SCAN_RESULT_RESTORE_FAILED', { objectId, sessionId, error: message });
          void clientDiagnostics.flush();
        }
      },
      (reason) => {
        if (cancelled || generation !== storageGeneration.current || decodeOwnerGeneration !== scanDecodeOwnerGeneration.current) return;
        scanLoadSession.current = key;
        const message = reason instanceof Error ? reason.message : String(reason);
        setScanReadRetryable(true);
        setScanStorageError(reason);
        setScanStoragePhase('error');
        void persistenceController.reconcile(objectId, sessionId, null);
        clientDiagnostics.warn('SCAN_RESULT_RESTORE_FAILED', { objectId, sessionId, error: message });
        void clientDiagnostics.flush();
      },
    );

    return () => {
      cancelled = true;
      cancelScanDecode('Saved scan load effect ended.');
    };
  }, [objectId, sessionId, scanLoadAttempt, persistenceController, selectMode, storageActionsDisabled, updateScanDurability, applyGeometry, publishRegistration, cancelScanDecode]);

  /* Retry loading the saved scan (and any sensor evidence that failed) without leaving the scanner screen. */
  const retryScanLoad = useCallback((): void => {
    if (objectId === null || sessionId === null) return;
    scanLoadSession.current = null;
    setScanReadRetryable(false);
    setScanStorageError(null);
    setScanStoragePhase('loading');
    setScanLoadAttempt((attempt) => attempt + 1);
  }, [objectId, sessionId]);

  const handleScanIntent = useCallback((intent: ScanStartIntent): boolean => {
    cancelScanDecode('A newer scanner intent was requested.');
    cancelScanEncode('A newer scanner intent was requested.');
    cancelContinuation('A newer scanner intent was requested.');
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
  }, [objectId, sessionId, scanDurability, publishRegistration, cancelContinuation, cancelScanEncode, cancelScanDecode]);

  /*
   * The launch ended and no immersive session is active. If Fresh already dropped the registration from memory but capture never began
   * (WebXR missing, permission denied, requestSession rejected), nothing was replaced: put the saved registration back (issue #23).
   */
  const handleScanLaunchAborted = useCallback((): void => {
    if (scanDurabilityRef.current.captureActive || !freshClearedRegistration.current) return;
    freshClearedRegistration.current = false;
    reloadSessionRegistration();
  }, [reloadSessionRegistration]);

  const handleSessionActiveChange = useCallback((active: boolean): void => {
    setXrSessionActive(active);
    if (!active) {
      cancelScanDecode('Scanner session ended.');
      cancelScanEncode('Scanner session ended.');
      cancelContinuation('Scanner session ended.', true);
    }
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
  }, [objectId, updateScanDurability, reloadSessionRegistration, cancelContinuation, cancelScanEncode, cancelScanDecode]);

  const handleScanReady = useCallback((incoming: ScanPointCloudSnapshot | null): void => {
    setScanStorageError(null);

    if (incoming === null) {
      setLiveTagAnchors([]); // a new scanner generation: anchors of the previous scan do not carry over
      setTagAnchorsDelivered(false);
      tagUploadKeyRef.current = null;
      setTagOwner({ baseId: scanDurabilityRef.current.durableId, frame: scanIntent.current === 'continue' ? 'pending' : 'fresh' });
      /*
       * The null callback marks XR startup. Keep the current durable scan and
       * its storage presentation intact until a replacement candidate exists.
       */
      const current = scanDurabilityRef.current;
      cancelScanDecode('A new scanner generation started.');
      cancelScanEncode('A new scanner generation started.');
      cancelContinuation('A new scanner generation started.');
      provisionalTag.current = { artifact: null, final: false };
      rejectedContinuation.current = false;
      registrationStage.current = null;
      updateScanDurability((state) => captureStarted(state, scanIntent.current));
      if (shouldShowScanningPresentation(current)) {
        setScanStoragePhase(objectId === null ? 'unbound' : 'scanning');
      }
      return;
    }

    freshClearedRegistration.current = false;
    const intentAtFinish = scanIntent.current;
    if (intentAtFinish === 'continue') {
      const base = continuationBase.current;
      if (base === null) {
        setScanStorageError(TranslatedError.fromKey('scan.error.continue_base_missing'));
        setScanStoragePhase('error');
        clientDiagnostics.warn('SCAN_CONTINUATION_REJECTED', { objectId, reason: 'missing durable base' });
        void clientDiagnostics.flush();
        return;
      }

      const previousPhase = scanStoragePhase;
      const generation = ++continuationOwnerGeneration.current;
      continuationPreviousPhase.current = previousPhase;
      continuationActive.current = true;
      setContinuationPhase('registration');
      setScanStoragePhase('continuing');
      const client = continuationClient.current ?? (continuationClient.current = new ContinuationClient());
      void client.continue(base, incoming, {
        generation,
        budgetLimitBytes: computeProfile === null
          ? CONTINUATION_TRANSIENT_BUDGET_BYTES
          : Math.min(CONTINUATION_TRANSIENT_BUDGET_BYTES, computeProfile.postScanMemoryBudgetBytes),
        onPhase: (phase) => {
          if (generation === continuationOwnerGeneration.current) setContinuationPhase(phase);
        },
      }).then((merged) => {
        if (generation !== continuationOwnerGeneration.current) return;
        continuationActive.current = false;
        setContinuationPhase(null);
        continuationPreviousPhase.current = null;
        if (!merged.ok) {
          setScanStoragePhase(previousPhase);
          setScanStorageError(TranslatedError.fromKey('scan.error.continue_not_saved', { reason: merged.reason }));
          clientDiagnostics.warn('SCAN_CONTINUATION_REJECTED', {
            objectId,
            reason: merged.reason,
            diagnostics: merged.diagnostics,
          });
          scanIntent.current = 'fresh';
          /* Keep the durable base and its registration available for another Continue attempt. */
          rejectedContinuation.current = true;
          void clientDiagnostics.flush();
          return;
        }
        clientDiagnostics.info('SCAN_CONTINUATION_REGISTERED', {
          objectId,
          inliers: merged.diagnostics.inliers,
          overlapPercent: Math.round(merged.diagnostics.overlapRatio * 100),
          rmsMm: Math.round(merged.diagnostics.rmsMeters * 1000),
          yawDegrees: Number((merged.diagnostics.yawRadians * 180 / Math.PI).toFixed(1)),
          translation: merged.diagnostics.translation.map((value) => Number(value.toFixed(3))),
          mergedStablePoints: merged.snapshot.pointCount,
          annotationsTruncated: merged.diagnostics.annotationsTruncated ?? false,
        });
        void clientDiagnostics.flush();
        setTagOwner((owner) => ({ ...owner, frame: merged.extensionToBase }));
        void finalizeScanReady(merged.snapshot, intentAtFinish, merged.extensionToBase);
      }).catch((reason: unknown) => {
        if (generation !== continuationOwnerGeneration.current) return;
        continuationActive.current = false;
        setContinuationPhase(null);
        continuationPreviousPhase.current = null;
        setScanStoragePhase(previousPhase);
        scanIntent.current = 'fresh';
        const message = reason instanceof Error ? reason.message : String(reason);
        setScanStorageError(reason instanceof ContinuationBudgetExceededError
          ? TranslatedError.fromKey('scan.continuation.memory_budget_exceeded', {
            estimate: formatNumber(locale, reason.estimate.estimatedBytes),
            limit: formatNumber(locale, reason.estimate.limitBytes),
          })
          : reason instanceof ContinuationCancelledError
            ? TranslatedError.fromKey('scan.continuation.cancelled')
            : TranslatedError.fromKey('scan.error.continue_not_saved', { reason: message }));
        clientDiagnostics.warn('SCAN_CONTINUATION_REJECTED', { objectId, reason: message });
        void clientDiagnostics.flush();
      });
      return;
    }

    void finalizeScanReady(incoming, intentAtFinish, null);

    async function finalizeScanReady(
      snapshot: ScanPointCloudSnapshot,
      intentAtFinish: ScanStartIntent,
      extensionToBase: RigidTransform | null,
    ): Promise<void> {

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
      if (intentAtFinish !== 'continue') {
        updateScanDurability((current) => confirmScanNotCommitted(beginProvisionalScan(current, snapshot, 'unbound'), current.durableSnapshot !== null));
      }
      setScanStorageError(TranslatedError.fromKey('scan.error.no_object_selected'));
      setScanStoragePhase('error');
      return;
    }

    const previousEncodePhase = scanEncodePreviousPhase.current ?? scanStoragePhase;
    cancelScanEncode('A newer scan finish superseded this encode.');
    const encodeGeneration = scanEncodeOwnerGeneration.current;
    const encodeOwner = { objectId, sessionId };
    const generation = ++storageGeneration.current;
    scanEncodePreviousPhase.current = previousEncodePhase;
    setScanStorageError(null);
    setScanStoragePhase('preparing');
    const encoder = scanEncodeClient.current ?? (scanEncodeClient.current = new ScanEncodeClient());
    let encoded: ScanEncodeResult;
    try {
      encoded = await encoder.encode(snapshot, {
        generation: encodeGeneration,
        budgetLimitBytes: computeProfile === null
          ? SCAN_ENCODE_TRANSIENT_COPY_BUDGET_BYTES
          : Math.min(SCAN_ENCODE_TRANSIENT_COPY_BUDGET_BYTES, computeProfile.postScanMemoryBudgetBytes),
        onFallback: () => {
          clientDiagnostics.warn('SCAN_ENCODE_SYNC_FALLBACK', {
            objectId, sessionId: scanSessionId, reason: 'Worker is unavailable',
          });
          void clientDiagnostics.flush();
        },
      });
    }
    catch (reason) {
      if (encodeGeneration !== scanEncodeOwnerGeneration.current ||
          scanEncodeOwner.current.objectId !== encodeOwner.objectId ||
          scanEncodeOwner.current.sessionId !== encodeOwner.sessionId ||
          generation !== storageGeneration.current ||
          reason instanceof ScanEncodeCancelledError) return;
      scanEncodePreviousPhase.current = null;
      const message = reason instanceof Error ? reason.message : String(reason);
      if (intentAtFinish !== 'continue') {
        updateScanDurability((current) => confirmScanNotCommitted(beginProvisionalScan(current, snapshot, 'unencoded'), current.durableSnapshot !== null));
      }
      setScanStorageError(TranslatedError.fromKey(message === SCAN_ANNOTATION_CAPACITY_ERROR
        ? SCAN_ANNOTATION_CAPACITY_ERROR
        : 'scan.error.prepare_failed'));
      setScanStoragePhase('error');
      clientDiagnostics.warn('SCAN_RESULT_SAVE_FAILED', { objectId, sessionId: scanSessionId, stage: 'encode', error: message });
      void clientDiagnostics.flush();
      return;
    }

    if (encodeGeneration !== scanEncodeOwnerGeneration.current ||
        scanEncodeOwner.current.objectId !== encodeOwner.objectId ||
        scanEncodeOwner.current.sessionId !== encodeOwner.sessionId ||
        generation !== storageGeneration.current) {
      releaseScanEncodeResult(encoded);
      return;
    }
    scanEncodePreviousPhase.current = null;
    const artifact = encoded.bytes;
    const identity = encoded.identity;

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
    if (intentAtFinish === 'fresh') continuationBaseRegistration.current = null;
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
            if (intentAtFinish === 'continue') continuationBaseRegistration.current = stage?.base ?? continuationBaseRegistration.current;
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
        continuationBaseRegistration.current = null;
        clientDiagnostics.info('SCAN_RESULT_SAVED', {
          objectId,
          sessionId: scanSessionId,
          bytes: identity.byteLength,
          stablePoints: snapshot.pointCount,
          manualPoints: snapshot.manualControlPoints.length,
          operatorEdges: snapshot.operatorEdges.length,
        });
        void clientDiagnostics.flush();
        if (sessionId === null && workflowScanSettled(state)) void onScanSaved?.();
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
    }
  }, [objectId, sessionId, scanSessionId, persistenceController, selectMode, storageActionsDisabled, updateScanDurability, reloadSessionRegistration, cancelContinuation, cancelScanEncode, cancelScanDecode, scanStoragePhase, computeProfile, locale, onScanSaved]);

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
        if (sessionId === null && workflowScanSettled(state)) void onScanSaved?.();
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
  }, [objectId, sessionId, scanSessionId, persistenceController, storageActionsDisabled, onScanSaved]);

  const deleteSavedScan = useCallback((): void => {
    if (storageActionsDisabled) return;
    cancelScanDecode('Saved scan deleted.');
    cancelScanEncode('Saved scan deleted.');
    cancelContinuation('Saved scan deleted.');
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
  }, [objectId, scanSessionId, persistenceController, storageActionsDisabled, updateScanDurability, publishRegistration, cancelContinuation, cancelScanEncode, cancelScanDecode]);

  const hasMeasuredScan = resultControlsAvailable(scanDurability) && scanSnapshot !== null && scanSnapshot.pointCount > 0;
  const durableViewport = scanViewportProvenance === 'durable' && scanAuthority !== null && scanAuthority === scanSnapshot;
  const viewportDurableScanId = scanSnapshot !== null && scanSnapshot === scanDurability.durableSnapshot
    ? scanDurability.durableId
    : null;
  const viewportScanIdentityKey = resultViewportKey({ objectId, durableScanId: viewportDurableScanId }, scanEncodeOwnerGeneration.current);
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
  /*
   * The tag layout is computed HERE, on the phone, and only the finished result goes to the ESP32 (tag-positions-v1.json beside the session's other
   * artifacts). It is saved once the scan itself is durable, so it can name the scan it belongs to. No layout (no MAIN, fewer than three tags, tags in
   * a line) removes an older file, because a new scan makes the previous positions stale. A failure here never touches the scan.
   */
  const tagAnchorsOwned = tagAnchorsDelivered && durableViewport && scanDurability.durableId !== null
    && tagOwner.frame !== 'pending' && (tagOwner.frame === 'fresh' || scanDurability.durableId !== tagOwner.baseId);
  /* Anchors in the frame of the scan on screen: an accepted continuation moved frame B into A, so they move with the merged cloud. */
  const scanFrameAnchors = useMemo(
    () => tagAnchorsOwned ? tagAnchorsToBaseFrame(liveTagAnchors, tagOwner.frame === 'fresh' || tagOwner.frame === 'pending' ? null : tagOwner.frame) : [],
    [tagAnchorsOwned, liveTagAnchors, tagOwner.frame],
  );
  useEffect(() => {
    if (!tagAnchorsOwned || objectId === null || scanDurability.durableId === null) return;
    const scanId = scanDurability.durableId;
    const built = buildTagLayout(scanFrameAnchors);
    const artifact = built.layout === null ? null : tagPositionsFromLayout(built.layout, scanId, 0, geometryGenerationRef.current);
    /*
     * An accepted continuation is a new scan id with A's tags still true in frame A: when the extension found nothing better, A's saved layout
     * is carried over (re-stamped with the merged id) instead of being dropped or replaced by a weaker one (issues #37, #38).
     */
    const inheriting = tagOwner.frame !== 'fresh' && tagOwner.frame !== 'pending';
    const stored = savedTagArtifactRef.current;
    const baseline = inheriting && stored !== null && stored.objectId === objectId && stored.sessionId === scanSessionId && stored.artifact.scan_frame_id === tagOwner.baseId
      ? stored.artifact : null;
    const baselineQuality = baseline === null ? null : { nodes: baseline.nodes.length, confirmed: baseline.nodes.filter((node) => node.confirmed).length };
    const keepBaseline = baseline !== null && !tagLayoutReplaces(built.layout, baselineQuality);
    const key = `${objectId}/${scanSessionId}/${scanId}/${keepBaseline ? 'inherit' : ''}${artifact === null ? `none:${built.reason}` : `${JSON.stringify(artifact.nodes)}@${artifact.geometry_computed_at_ms ?? 'no-epoch'}`}`;
    if (tagUploadKeyRef.current === key) return;
    tagUploadKeyRef.current = key;
    const savedForScan = savedTagLayoutRef.current?.scanId === scanId ? savedTagLayoutRef.current : null;
    if (!keepBaseline && !tagLayoutReplaces(built.layout, savedForScan)) {
      clientDiagnostics.info('TAG_POSITIONS_KEPT', {
        objectId, sessionId: scanSessionId, saved: `${savedForScan?.confirmed}/${savedForScan?.nodes}`,
        candidate: built.layout === null ? `none:${built.reason}` : `${tagLayoutQuality(built.layout).confirmed}/${built.layout.nodes.length}`,
      });
      void clientDiagnostics.flush();
      return;
    }
    void (async () => {
      try {
        if (keepBaseline && baseline !== null && baselineQuality !== null) {
          const inherited: TagPositionsArtifact = { ...baseline, scan_frame_id: scanId, created_at_ms: Date.now() };
          const result = await saveSessionTagPositions(objectId, scanSessionId, encodeTagPositions(inherited));
          if (result?.ok !== true) throw new Error(result?.error ?? 'tag_positions_rejected');
          savedTagArtifactRef.current = { objectId, sessionId: scanSessionId, artifact: inherited };
          savedTagLayoutRef.current = { scanId, ...baselineQuality };
          clientDiagnostics.info('TAG_POSITIONS_INHERITED', { objectId, sessionId: scanSessionId, from: baseline.scan_frame_id, nodes: baseline.nodes.length, candidate: built.layout === null ? `none:${built.reason}` : 'weaker' });
        } else if (built.layout === null) {
          /* A failed delete leaves the previous sidecar behind: that is a failure to retry, never a quiet "no tags". */
          await deleteSessionTagPositions(objectId, scanSessionId);
          clientDiagnostics.info('TAG_POSITIONS_NONE', { objectId, sessionId: scanSessionId, reason: built.reason, anchors: scanFrameAnchors.length, excluded: built.excluded.map((item) => `${item.node}:${item.reason}`).join(',') });
        } else {
          const toSave = tagPositionsFromLayout(built.layout, scanId, Date.now(), geometryGenerationRef.current);
          const result = await saveSessionTagPositions(objectId, scanSessionId, encodeTagPositions(toSave));
          if (result?.ok !== true) throw new Error(result?.error ?? 'tag_positions_rejected');
          savedTagArtifactRef.current = { objectId, sessionId: scanSessionId, artifact: toSave };
          savedTagLayoutRef.current = { scanId, ...tagLayoutQuality(built.layout) };
          clientDiagnostics.info('TAG_POSITIONS_SAVED', {
            objectId, sessionId: scanSessionId, nodes: built.layout.nodes.map((node) => `${node.nodeId}${node.confirmed ? '' : '?'}`).join(','),
            triangles: built.layout.triangles.length, weakestMm: built.layout.weakestMainTriangleHeightMm, excluded: built.excluded.map((item) => `${item.node}:${item.reason}`).join(','),
          });
        }
        tagRetryAttempts.current = 0;
      } catch (reason) {
        clientDiagnostics.warn('TAG_POSITIONS_SAVE_FAILED', { objectId, sessionId: scanSessionId, error: reason instanceof Error ? reason.message : String(reason), attempt: tagRetryAttempts.current + 1 });
        /* Only the owner of the latest attempt may reopen it: a newer key (other scan, anchors or object) keeps its own state. */
        if (tagUploadKeyRef.current === key) {
          tagUploadKeyRef.current = null;
          if (tagRetryAttempts.current < TAG_UPLOAD_MAX_RETRIES) {
            const delayMs = TAG_UPLOAD_RETRY_BASE_MS * 3 ** tagRetryAttempts.current;
            tagRetryAttempts.current += 1;
            if (tagRetryTimer.current !== null) clearTimeout(tagRetryTimer.current);
            tagRetryTimer.current = setTimeout(() => { tagRetryTimer.current = null; setTagRetryTick((tick) => tick + 1); }, delayMs);
          }
        }
      }
      void clientDiagnostics.flush();
    })();
  }, [tagAnchorsOwned, objectId, scanSessionId, scanDurability.durableId, scanFrameAnchors, tagRetryTick, tagOwner, localizationGeometry]);
  useEffect(() => () => { if (tagRetryTimer.current !== null) clearTimeout(tagRetryTimer.current); }, []);
  /* Tags confirmed during the live scan appear on the result for this scan; saved registration / geometry markers keep their nodes. */
  const resultSensorMarkers = useMemo(
    () => durableViewport ? mergeLiveTagAnchorMarkers(sensorPlacementScene.markers, scanFrameAnchors) : [],
    [durableViewport, sensorPlacementScene, scanFrameAnchors],
  );
  /* Distances between the devices recalculated from the scan: only from a registration that applies to the scan on screen. */
  const distanceRefinement = useMemo(
    () => computeScanDistanceRefinement(durableViewport ? localizationGeometry : null, viewerRegistration),
    [durableViewport, localizationGeometry, viewerRegistration],
  );
  const registrationStatusText = registrationStageView === 'pending' ? t('scan.registration.state.pending')
    : registrationStageView === 'saving' ? t('scan.registration.state.saving')
      : registrationStageView === 'failed' ? t('scan.registration.state.failed')
        : registrationFit === null ? t('scan.registration.state.none')
          : registrationFit.ok ? t('scan.registration.state.saved')
            : t('scan.registration.state.stale', { reason: t(REGISTRATION_INAPPLICABLE_KEYS[registrationFit.reason]) });
  const continuationPhaseLabel = continuationPhase === null ? null : ({
    registration: t('scan.continuation.phase.registration'),
    stable: t('scan.continuation.phase.stable'),
    occupancy: t('scan.continuation.phase.occupancy'),
    tsdf: t('scan.continuation.phase.tsdf'),
  } as const)[continuationPhase];
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
  const storageBusy = storageActionsDisabled || scanStoragePhase === 'loading' || scanStoragePhase === 'preparing' || scanStoragePhase === 'saving' || scanStoragePhase === 'deleting';
  const localizationDataReady = localizationReady(scanDurability) &&
    ((scanPersistence?.scan === 'saved' && scanPersistence.occupancy === 'ready') || previousKeptProvable(scanDurability));

  return (
    <main className={!xrSessionActive && hasMeasuredScan ? 'app-shell app-shell--result' : 'app-shell'}>
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
        <div className="xr-runtime-error" role="alert">
          {formatErrorText(locale, scanStorageError)}
          {scanReadRetryable && (
            <button type="button" className="scan-clear-button" data-testid="retry-scan-load" disabled={storageActionsDisabled} onClick={retryScanLoad}>
              {t('scan.retry.load')}
            </button>
          )}
        </div>
      )}
      {!xrSessionActive && continuationPhaseLabel !== null && (
        <div className="xr-runtime-warning" role="status" aria-live="polite">{continuationPhaseLabel}</div>
      )}
      {xrSessionActive && (scanStoragePhase === 'preparing' || scanStoragePhase === 'reading') && (
        <div className="xr-runtime-warning" role="status" aria-live="polite">
          {scanStorageLabel(scanStoragePhase, objectId, locale)}
        </div>
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

          <details className="scan-more-controls">
            <summary>{t('scan.controls.more')}</summary>
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
          </details>
        </section>
      )}

      <XrRuntimePanel
        objectId={objectId}
        sessionId={sessionId}
        savedScanAvailable={storageGood && durableViewport && hasMeasuredScan}
        savedScanBusy={storageBusy}
        scanLaunchReady={(scanStoragePhase === 'none' || (sessionId === null && objectId !== null)) && !savedScanAvailable(scanDurability)}
        onScanIntent={handleScanIntent}
        onDeleteSavedScan={deleteSavedScan}
        onSessionActiveChange={handleSessionActiveChange}
        onScanLaunchAborted={handleScanLaunchAborted}
        onPointCloudReady={handleScanReady}
        onCapabilitiesChange={setXrCapabilities}
        onRegistrationChange={onRegistrationChange}
        scanIntent={scanIntentState}
        baseScanFrameId={scanDurability.durableId}
        onTagRegistrationResult={onTagRegistrationResult}
        onTagAnchors={(anchors) => { setLiveTagAnchors(anchors); setTagAnchorsDelivered(true); }}
      />

      {!xrSessionActive && hasMeasuredScan && scanSnapshot !== null && (
        <section className="workspace" data-scan-provenance={scanViewportProvenance}>
          {scanViewportProvenance === 'provisional' && (
            <div className="xr-runtime-warning" role="status" data-scan-provenance="provisional">
              {t('scan.storage.provisional')}
            </div>
          )}
          <ViewportFailureBoundary
            key={viewportScanIdentityKey}
            diagnosticEvent="SCAN_RESULT_RENDERER_LOAD_FAILED"
            renderFailure={({ message, attempts, retry }) => (
              <div className="viewport-shell scan-viewport" role="alert">
                <div>{t('scan.renderer.failed', { error: message })}</div>
                {attempts < VIEWPORT_FAILURE_MAX_ATTEMPTS && (
                  <button type="button" onClick={retry}>{t('scan.renderer.retry')}</button>
                )}
              </div>
            )}
          >
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
                sensorMarkers={resultSensorMarkers}
                sensorGeometry={durableViewport ? localizationGeometry : null}
                registrationArtifact={viewerRegistration}
                currentScanId={durableViewport ? scanDurability.durableId : null}
                scanIdentityKey={viewportScanIdentityKey}
                objectId={objectId}
                sessionId={sessionId}
                persistenceBusy={scanStoragePhase === 'preparing' || scanStoragePhase === 'saving'}
                postScanGeneration={storageGeneration.current}
                computeProfile={computeProfile}
                localizationRuns={durableViewport && localizationRunListing?.object_id === objectId && localizationRunListing.session_id === sessionId
                  ? localizationRunListing.runs
                  : undefined}
              />
            </Suspense>
          </ViewportFailureBoundary>

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
            <ScanDistanceRefinementCard refinement={distanceRefinement} locale={locale} />
          </aside>
        </section>
      )}
    </main>
  );
}
