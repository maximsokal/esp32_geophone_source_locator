import { Component, lazy, Suspense, useCallback, useEffect, useMemo, useRef, useState, type ErrorInfo, type ReactNode } from 'react';

import './scan.css';

import {
  deleteObjectScanArtifact,
  fetchLocalizationRuns,
  fetchScanInfo,
  loadObjectScanArtifact,
  loadSessionRegistration,
  saveObjectScanArtifact,
  saveSessionOccupancyArtifact,
  saveSessionPreviewArtifact,
} from './api/deviceApi';
import type { LocalizationSessionGeometry } from './api/deviceApi';
import type { DeviceStatus } from './domain/device';
import { buildSessionOccupancyArtifact } from './xr/occupancySolverArtifact';
import { buildVoxelPreviewArtifact } from './xr/voxelPreview';
import { decodeRegistrationArtifact, toOccupancyRegistration, type RegistrationArtifact } from './xr/fiducial/registrationArtifact';
import { ScanPersistenceController, SCAN_STORAGE_BLOCKED_MESSAGE, type ScanPersistenceState } from './xr/scanPersistence';
import { scannerStorageActionsDisabled } from './features/objects/measurementTransition';
import type { ViewCommand, ViewPreset } from './components/SceneViewport';
import { XrRuntimePanel, type ScanStartIntent } from './components/XrRuntimePanel';
import { clientDiagnostics } from './data/clientDiagnostics';
import { countCoverageQualityBands } from './xr/coverageQuality';
import {
  deviceComputeProfilePromise,
  type DeviceComputeProfile,
} from './xr/deviceComputeProfile';
import { decodeScanArtifact, encodeScanArtifact } from './xr/scanArtifact';
import { buildSensorPlacementScene } from './xr/sensorPlacementScene';
import { registerAndMergeScanContinuation } from './xr/scanContinuation';
import {
  displayForScanMode,
  scanViewForMode,
  toggleScanLayer,
  type ScanDisplayOptions,
  type ScanMode,
} from './xr/scanDisplayModes';
import type { ScanPointCloudSnapshot, WebXrCapabilities } from './xr/webxrRuntime';
import { translate, useLocale, useT, type I18nLocale } from './i18n';
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
  if (persistence?.scan !== 'saved') return translate(locale, 'scan.occupancy.checking');
  const keys = {
    pending: 'scan.occupancy.pending', saving: 'scan.occupancy.saving', ready: 'scan.occupancy.ready',
    failed: 'scan.occupancy.failed', unavailable: 'scan.occupancy.unavailable',
  } as const;
  return translate(locale, keys[persistence.occupancy]);
}

function previewStatusLabel(persistence: ScanPersistenceState | null, locale: I18nLocale): string {
  if (persistence?.scan !== 'saved') return translate(locale, 'scan.preview.checking');
  const keys = {
    pending: 'scan.preview.pending', saving: 'scan.preview.saving', ready: 'scan.preview.ready',
    failed: 'scan.preview.failed', unavailable: 'scan.preview.unavailable',
  } as const;
  return translate(locale, keys[persistence.preview]);
}

function persistedScanStorageLabel(
  phase: ScanStoragePhase,
  objectId: string | null,
  persistence: ScanPersistenceState | null,
  locale: I18nLocale,
): string {
  const scanLabel = scanStorageLabel(phase, objectId, locale);
  if (phase !== 'saved' && phase !== 'loaded') return scanLabel;
  return `${scanLabel} | ${occupancyStatusLabel(persistence, locale)}`;
}

export default function App({ objectId, sessionId, status }: AppProps) {
  const { locale } = useLocale();
  const t = useT();
  const storageActionsDisabled = scannerStorageActionsDisabled(status);
  const [xrSessionActive, setXrSessionActive] = useState(false);
  const [xrCapabilities, setXrCapabilities] = useState<WebXrCapabilities | null>(null);
  const [computeProfile, setComputeProfile] = useState<DeviceComputeProfile | null>(null);
  const [scanSnapshot, setScanSnapshot] = useState<ScanPointCloudSnapshot | null>(null);
  const [scanStoragePhase, setScanStoragePhase] = useState<ScanStoragePhase>(
    objectId === null || sessionId === null ? 'unbound' : 'loading',
  );
  const [scanStorageError, setScanStorageError] = useState<string | null>(null);
  const [scanPersistence, setScanPersistence] = useState<ScanPersistenceState | null>(null);
  const [localizationGeometry, setLocalizationGeometry] = useState<LocalizationSessionGeometry | null>(null);
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
  const registrationForSession = useRef<{ objectId: string; sessionId: string; artifact: RegistrationArtifact } | null>(null);
  const onRegistrationChange = useCallback((artifact: RegistrationArtifact | null): void => {
    if (artifact === null || objectId === null || sessionId === null) {
      registrationForSession.current = null;
      setRegistrationArtifact(null);
      return;
    }
    registrationForSession.current = { objectId, sessionId, artifact };
    setRegistrationArtifact(artifact);
  }, [objectId, sessionId]);
  useEffect(() => {
    registrationForSession.current = null;
    setLocalizationGeometry(null);
    setRegistrationArtifact(null);
  }, [objectId, sessionId]);
  const scanIntent = useRef<ScanStartIntent>('fresh');
  const continuationBase = useRef<ScanPointCloudSnapshot | null>(null);
  const scanPersistenceController = useRef<ScanPersistenceController | null>(null);
  if (scanPersistenceController.current === null) {
    scanPersistenceController.current = new ScanPersistenceController({
      saveScan: saveObjectScanArtifact,
      saveOccupancy: saveSessionOccupancyArtifact,
      savePreview: saveSessionPreviewArtifact,
      fetchInfo: fetchScanInfo,
    }, setScanPersistence);
  }
  const persistenceController = scanPersistenceController.current;

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
    scanLoadSession.current = null;
    sessionEvidenceLoad.current = null;
    scanIntent.current = 'fresh';
    continuationBase.current = null;
    setScanSnapshot(null);
    setScanStorageError(null);
    setLocalizationGeometry(null);
    setRegistrationArtifact(null);

    if (objectId === null || sessionId === null) {
      setScanStoragePhase('unbound');
      return undefined;
    }

    setScanStoragePhase('loading');
    return () => { storageGeneration.current += 1; };
  }, [objectId, sessionId, persistenceController]);

  useEffect(() => {
    if (objectId === null || sessionId === null || storageActionsDisabled) return undefined;
    const key = `${objectId}/${sessionId}`;
    const shouldLoadScan = scanLoadSession.current !== key;
    const shouldLoadEvidence = sessionEvidenceLoad.current !== key;
    if (!shouldLoadScan && !shouldLoadEvidence) return undefined;
    const generation = storageGeneration.current;
    let cancelled = false;
    if (shouldLoadEvidence) {
      void Promise.all([
        fetchLocalizationRuns(objectId, sessionId).then(
          (listing) => ({ ok: true as const, geometry: listing.geometry }),
          (reason: unknown) => ({ ok: false as const, reason }),
        ),
        loadSessionRegistration(objectId, sessionId).then(
          (json) => ({ ok: true as const, json }),
          (reason: unknown) => ({ ok: false as const, reason }),
        ),
      ]).then(([geometryResult, registrationResult]) => {
        if (cancelled || generation !== storageGeneration.current) return;
        sessionEvidenceLoad.current = key;
        if (geometryResult.ok) setLocalizationGeometry(geometryResult.geometry);
        else {
          const message = geometryResult.reason instanceof Error ? geometryResult.reason.message : String(geometryResult.reason);
          setLocalizationGeometry(null);
          clientDiagnostics.warn('SENSOR_GEOMETRY_RESTORE_FAILED', { objectId, sessionId, error: message });
        }
        if (!registrationResult.ok) {
          const message = registrationResult.reason instanceof Error ? registrationResult.reason.message : String(registrationResult.reason);
          setRegistrationArtifact(null);
          clientDiagnostics.warn('SENSOR_REGISTRATION_RESTORE_FAILED', { objectId, sessionId, error: message });
        } else if (registrationResult.json === null) setRegistrationArtifact(null);
        else {
          try { setRegistrationArtifact(decodeRegistrationArtifact(registrationResult.json)); }
          catch (reason) {
            const message = reason instanceof Error ? reason.message : String(reason);
            setRegistrationArtifact(null);
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

        void persistenceController.reconcile(objectId, sessionId, artifact);

        try {
          const restored = decodeScanArtifact(artifact);
          if (restored.pointCount <= 0) throw new Error('saved artifact has no stable measured surface');
          scanLoadSession.current = key;
          setScanSnapshot(restored);
          selectMode('quality');
          setScanStoragePhase('loaded');
          clientDiagnostics.info('SCAN_RESULT_RESTORED', {
            objectId,
            sessionId,
            bytes: artifact.byteLength,
            stablePoints: restored.pointCount,
            tentativePoints: restored.tentativePointCount,
            manualPoints: restored.manualControlPoints.length,
            operatorEdges: restored.operatorEdges.length,
          });
          void clientDiagnostics.flush();
        }
        catch (reason) {
          scanLoadSession.current = key;
          const message = reason instanceof Error ? reason.message : String(reason);
          setScanStorageError(message);
          setScanStoragePhase('error');
          clientDiagnostics.warn('SCAN_RESULT_RESTORE_FAILED', { objectId, sessionId, error: message });
          void clientDiagnostics.flush();
        }
      },
      (reason) => {
        if (cancelled || generation !== storageGeneration.current) return;
        scanLoadSession.current = key;
        const message = reason instanceof Error ? reason.message : String(reason);
        setScanStorageError(message);
        setScanStoragePhase('error');
        void persistenceController.reconcile(objectId, sessionId, null);
        clientDiagnostics.warn('SCAN_RESULT_RESTORE_FAILED', { objectId, sessionId, error: message });
        void clientDiagnostics.flush();
      },
    );

    return () => { cancelled = true; };
  }, [objectId, sessionId, persistenceController, selectMode, storageActionsDisabled]);

  const handleScanIntent = useCallback((intent: ScanStartIntent): boolean => {
    setScanStorageError(null);
    if (intent === 'continue') {
      const durable = scanStoragePhase === 'saved' || scanStoragePhase === 'loaded';
      if (!durable || scanSnapshot === null || scanSnapshot.pointCount <= 0) {
        setScanStorageError('No durable saved 3D scan is available to continue. Open or finish a scan first.');
        return false;
      }
      continuationBase.current = scanSnapshot;
      scanIntent.current = 'continue';
      clientDiagnostics.info('SCAN_CONTINUATION_REQUEST', {
        objectId,
        stablePoints: scanSnapshot.pointCount,
        freeVoxels: scanSnapshot.knownFreeVoxelCount,
        unknownVoxels: scanSnapshot.unknownCandidateVoxelCount,
      });
      void clientDiagnostics.flush();
      return true;
    }

    scanIntent.current = 'fresh';
    continuationBase.current = null;
    clientDiagnostics.info('SCAN_REPLACEMENT_REQUEST', {
      objectId,
      previousSavedScanKeptUntilCommit: scanStoragePhase === 'saved' || scanStoragePhase === 'loaded',
    });
    void clientDiagnostics.flush();
    return true;
  }, [objectId, scanSnapshot, scanStoragePhase]);

  const handleScanReady = useCallback((incoming: ScanPointCloudSnapshot | null): void => {
    setScanStorageError(null);

    if (incoming === null) {
      /*
       * Starting a continuation must not make the already durable result vanish
       * from parent state. WebXR uses a fresh local frame; the saved base remains
       * authoritative until the extension is successfully registered and saved.
       */
      if (scanIntent.current === 'continue' && continuationBase.current !== null) {
        setScanSnapshot(continuationBase.current);
      }
      else {
        setScanSnapshot(null);
      }
      setScanStoragePhase(objectId === null || sessionId === null ? 'unbound' : 'scanning');
      return;
    }

    let snapshot = incoming;
    if (scanIntent.current === 'continue') {
      const base = continuationBase.current;
      if (base === null) {
        setScanStorageError('Continuation finished without its saved base; nothing was overwritten.');
        setScanStoragePhase('error');
        clientDiagnostics.warn('SCAN_CONTINUATION_REJECTED', { objectId, reason: 'missing durable base' });
        void clientDiagnostics.flush();
        return;
      }

      const merged = registerAndMergeScanContinuation(base, incoming);
      if (!merged.ok) {
        setScanSnapshot(base);
        setScanStoragePhase('loaded');
        setScanStorageError(`Continuation NOT saved. ${merged.reason}`);
        clientDiagnostics.warn('SCAN_CONTINUATION_REJECTED', {
          objectId,
          reason: merged.reason,
          diagnostics: merged.diagnostics,
        });
        scanIntent.current = 'fresh';
        continuationBase.current = null;
        void clientDiagnostics.flush();
        return;
      }

      snapshot = merged.snapshot;
      clientDiagnostics.info('SCAN_CONTINUATION_REGISTERED', {
        objectId,
        inliers: merged.diagnostics.inliers,
        overlapPercent: Math.round(merged.diagnostics.overlapRatio * 100),
        rmsMm: Math.round(merged.diagnostics.rmsMeters * 1000),
        yawDegrees: Number((merged.diagnostics.yawRadians * 180 / Math.PI).toFixed(1)),
        translation: merged.diagnostics.translation.map((value) => Number(value.toFixed(3))),
        mergedStablePoints: snapshot.pointCount,
      });
      void clientDiagnostics.flush();
    }

    setScanSnapshot(snapshot);
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

    if (objectId === null || sessionId === null) {
      setScanStorageError('No recording/session is open; the completed scan exists only in browser memory.');
      setScanStoragePhase('error');
      return;
    }

    let artifact: Uint8Array;
    try {
      artifact = encodeScanArtifact(snapshot);
    }
    catch (reason) {
      const message = reason instanceof Error ? reason.message : String(reason);
      setScanStorageError(message);
      setScanStoragePhase('error');
      clientDiagnostics.warn('SCAN_RESULT_SAVE_FAILED', { objectId, sessionId, stage: 'encode', error: message });
      void clientDiagnostics.flush();
      return;
    }

    const generation = ++storageGeneration.current;
    setScanStoragePhase('saving');
    void persistenceController.save(
      objectId,
      sessionId,
      artifact,
      () => {
        const registration = registrationForSession.current;
        const occupancyRegistration = registration?.objectId === objectId && registration.sessionId === sessionId
          ? toOccupancyRegistration(registration.artifact)
          : null;
        return buildSessionOccupancyArtifact(snapshot, artifact, Date.now(), occupancyRegistration);
      },
      (occupancyArtifact) => {
        const registration = registrationForSession.current;
        const occupancyRegistration = registration?.objectId === objectId && registration.sessionId === sessionId
          ? toOccupancyRegistration(registration.artifact)
          : null;
        return buildVoxelPreviewArtifact(snapshot, artifact, { occupancyArtifact, registration: occupancyRegistration });
      },
      storageActionsDisabled,
    ).then((state) => {
        if (generation !== storageGeneration.current) return;
        if (state?.scan !== 'saved') {
          const message = state?.scanError ?? SCAN_STORAGE_BLOCKED_MESSAGE;
          setScanStorageError(`New scan is only in browser memory; previous SD scan was kept. ${message}`);
          setScanStoragePhase('error');
          clientDiagnostics.warn('SCAN_RESULT_SAVE_FAILED', { objectId, sessionId, stage: 'transport', error: message });
          void clientDiagnostics.flush();
          return;
        }
        setScanStoragePhase('saved');
        setScanStorageError(null);
        scanIntent.current = 'fresh';
        continuationBase.current = null;
        clientDiagnostics.info('SCAN_RESULT_SAVED', {
          objectId,
          sessionId,
          bytes: artifact.byteLength,
          stablePoints: snapshot.pointCount,
          manualPoints: snapshot.manualControlPoints.length,
          operatorEdges: snapshot.operatorEdges.length,
        });
        void clientDiagnostics.flush();
        if (state.occupancy === 'ready') {
          clientDiagnostics.info('SCAN_OCCUPANCY_SAVED', { objectId, sessionId });
        } else if (state.occupancy === 'failed' || state.occupancy === 'unavailable') {
          clientDiagnostics.warn('SCAN_OCCUPANCY_SAVE_FAILED', {
            objectId, sessionId, error: state.occupancyError,
          });
          void clientDiagnostics.flush();
        }
        if (state.preview === 'ready') {
          clientDiagnostics.info('SCAN_VOXEL_PREVIEW_SAVED', { objectId, sessionId });
        } else if (state.preview === 'failed' || state.preview === 'unavailable') {
          clientDiagnostics.warn('SCAN_VOXEL_PREVIEW_SAVE_FAILED', {
            objectId, sessionId, error: state.previewError,
          });
          void clientDiagnostics.flush();
        }
    });
  }, [objectId, sessionId, persistenceController, selectMode, storageActionsDisabled]);

  const retryPersistence = useCallback((): void => {
    if (storageActionsDisabled || objectId === null || sessionId === null) return;
    const generation = storageGeneration.current;
    void persistenceController.retry(objectId, sessionId).then((state) => {
      if (generation !== storageGeneration.current || state === null) return;
      if (state.scan === 'saved') {
        setScanStoragePhase('saved');
        setScanStorageError(null);
        scanIntent.current = 'fresh';
        continuationBase.current = null;
      } else if (state.scan === 'blocked_recording') {
        setScanStorageError(state.scanError ?? SCAN_STORAGE_BLOCKED_MESSAGE);
      } else if (state.scan === 'error') {
        setScanStorageError(state.scanError);
      }
      const event = state.scan === 'blocked_recording' || state.scan === 'error'
        ? 'SCAN_RESULT_SAVE_FAILED'
        : state.occupancy === 'ready' ? 'SCAN_OCCUPANCY_SAVED'
          : state.preview === 'ready' ? 'SCAN_VOXEL_PREVIEW_SAVED'
            : state.preview === 'failed' || state.preview === 'unavailable' ? 'SCAN_VOXEL_PREVIEW_SAVE_FAILED'
              : 'SCAN_OCCUPANCY_SAVE_FAILED';
      clientDiagnostics.info(event, { objectId, sessionId, occupancyError: state.occupancyError, previewError: state.previewError });
      void clientDiagnostics.flush();
    });
  }, [objectId, sessionId, persistenceController, storageActionsDisabled]);

  const deleteSavedScan = useCallback((): void => {
    if (storageActionsDisabled) return;
    scanIntent.current = 'fresh';
    continuationBase.current = null;
    persistenceController.reset();
    if (objectId === null || sessionId === null) {
      setScanSnapshot(null);
      setScanStoragePhase('unbound');
      return;
    }
    const generation = ++storageGeneration.current;
    setScanStoragePhase('deleting');
    setScanStorageError(null);
    void deleteObjectScanArtifact(objectId, sessionId).then(
      () => {
        if (generation !== storageGeneration.current) return;
        setScanSnapshot(null);
        setScanStoragePhase('none');
        clientDiagnostics.info('SCAN_RESULT_DELETED', { objectId, sessionId });
        void clientDiagnostics.flush();
      },
      (reason) => {
        if (generation !== storageGeneration.current) return;
        const message = reason instanceof Error ? reason.message : String(reason);
        setScanStorageError(message);
        setScanStoragePhase('error');
        clientDiagnostics.warn('SCAN_RESULT_DELETE_FAILED', { objectId, sessionId, error: message });
        void clientDiagnostics.flush();
      },
    );
  }, [objectId, sessionId, persistenceController, storageActionsDisabled]);

  const hasMeasuredScan = scanSnapshot !== null && scanSnapshot.pointCount > 0;
  const sensorPlacementScene = useMemo(
    () => buildSensorPlacementScene(localizationGeometry, registrationArtifact),
    [localizationGeometry, registrationArtifact],
  );
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
  const storageGood = scanStoragePhase === 'saved' || scanStoragePhase === 'loaded';
  const storageBusy = storageActionsDisabled || scanStoragePhase === 'loading' || scanStoragePhase === 'saving' || scanStoragePhase === 'deleting';
  const localizationDataReady = scanPersistence?.scan === 'saved' && scanPersistence.occupancy === 'ready';

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
        <div className="xr-runtime-error" role="alert">{scanStorageError === SCAN_STORAGE_BLOCKED_MESSAGE ? t('scan.storage.paused') : scanStorageError}</div>
      )}
      {storageActionsDisabled && (
        <div className="xr-runtime-warning" role="status">{t('scan.storage.paused')}</div>
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
        savedScanAvailable={storageGood && hasMeasuredScan}
        savedScanBusy={storageBusy}
        onScanIntent={handleScanIntent}
        onDeleteSavedScan={deleteSavedScan}
        onSessionActiveChange={setXrSessionActive}
        onPointCloudReady={handleScanReady}
        onCapabilitiesChange={setXrCapabilities}
        onRegistrationChange={onRegistrationChange}
      />

      {!xrSessionActive && hasMeasuredScan && scanSnapshot !== null && (
        <section className="workspace">
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
                sensorMarkers={sensorPlacementScene.markers}
                sensorGeometry={localizationGeometry}
                registrationArtifact={registrationArtifact}
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
                (scanPersistence.occupancy === 'failed' || scanPersistence.occupancy === 'unavailable') && (
                <p className="scan-volume-note" role="status">
                  {scanPersistence.occupancyError ?? t('scan.volume.occupancy_not_ready')}
                </p>
              )}

              {scanPersistence?.scan === 'saved' &&
                (scanPersistence.preview === 'failed' || scanPersistence.preview === 'unavailable') && (
                <p className="scan-volume-note" role="status">
                  {scanPersistence.previewError ?? t('scan.volume.preview_not_ready')}
                </p>
              )}

              {persistenceController.retryAvailable && (
                <button type="button" className="scan-clear-button" disabled={storageActionsDisabled || scanPersistence?.scan === 'saving' || scanPersistence?.occupancy === 'saving' || scanPersistence?.preview === 'saving'} onClick={retryPersistence}>
                  {scanPersistence?.scan === 'blocked_recording' ? t('scan.retry.scan') : scanPersistence?.occupancy === 'failed' ? t('scan.retry.occupancy') : t('scan.retry.preview')}
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
