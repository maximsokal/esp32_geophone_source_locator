import { Component, lazy, Suspense, useCallback, useEffect, useRef, useState, type ErrorInfo, type ReactNode } from 'react';

import './scan.css';

import {
  deleteObjectScanArtifact,
  fetchScanInfo,
  loadObjectScanArtifact,
  saveObjectScanArtifact,
  saveSessionOccupancyArtifact,
} from './api/deviceApi';
import { buildSessionOccupancyArtifact } from './xr/occupancySolverArtifact';
import { toOccupancyRegistration, type RegistrationArtifact } from './xr/fiducial/registrationArtifact';
import { ScanPersistenceController, type ScanPersistenceState } from './xr/scanPersistence';
import type { ViewCommand, ViewPreset } from './components/SceneViewport';
import { XrRuntimePanel, type ScanStartIntent } from './components/XrRuntimePanel';
import { clientDiagnostics } from './data/clientDiagnostics';
import { countCoverageQualityBands } from './xr/coverageQuality';
import {
  deviceComputeProfilePromise,
  type DeviceComputeProfile,
} from './xr/deviceComputeProfile';
import { decodeScanArtifact, encodeScanArtifact } from './xr/scanArtifact';
import { registerAndMergeScanContinuation } from './xr/scanContinuation';
import {
  displayForScanMode,
  scanViewForMode,
  toggleScanLayer,
  type ScanDisplayOptions,
  type ScanMode,
} from './xr/scanDisplayModes';
import type { ScanPointCloudSnapshot, WebXrCapabilities } from './xr/webxrRuntime';

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
}

class ResultViewportErrorBoundary extends Component<
  { children: ReactNode },
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
          3D result renderer failed: {this.state.message}
        </div>
      );
    }
    return this.props.children;
  }
}

function formatMeters(value: number): string {
  return `${value.toFixed(2)} m`;
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

function computeProfileTitle(profile: DeviceComputeProfile): string {
  return [
    `${profile.reportedLogicalProcessors} browser-reported logical processors`,
    `${profile.liveWorkerBudget} heavy live XR worker slot(s)`,
    `${profile.postScanWorkerBudget} post-scan worker slot(s)`,
    `shared memory ${profile.sharedMemory ? 'available' : 'unavailable'}`,
    `Wasm threads ${profile.wasmThreads ? 'available' : 'unavailable'}`,
    `WebGPU ${profile.webGpu ? 'available' : 'unavailable'}`,
    `tier ${profile.tier}`,
  ].join(' · ');
}

function scanStorageLabel(phase: ScanStoragePhase, objectId: string | null): string {
  const objectLabel = objectId ?? 'NO OBJECT';
  switch (phase) {
    case 'loading': return `LOADING SAVED 3D · ${objectLabel}`;
    case 'none': return `NO SAVED 3D SCAN · ${objectLabel}`;
    case 'scanning': return `NEW SCAN IN MEMORY · ${objectLabel}`;
    case 'saving': return `SAVING 3D TO SD · ${objectLabel}`;
    case 'saved': return `3D SAVED TO SD · ${objectLabel}`;
    case 'loaded': return `3D RESTORED FROM SD · ${objectLabel}`;
    case 'deleting': return `DELETING SAVED 3D · ${objectLabel}`;
    case 'error': return `3D STORAGE ERROR · ${objectLabel}`;
    default: return '3D SCAN NOT BOUND TO AN OBJECT';
  }
}

function occupancyStatusLabel(persistence: ScanPersistenceState | null): string {
  let occupancyLabel = 'OCCUPANCY STATUS CHECKING';
  if (persistence?.scan === 'saved') {
    switch (persistence.occupancy) {
      case 'pending': occupancyLabel = 'OCCUPANCY PENDING'; break;
      case 'saving': occupancyLabel = 'OCCUPANCY SAVING'; break;
      case 'ready': occupancyLabel = 'LOCALIZATION DATA READY'; break;
      case 'failed': occupancyLabel = 'OCCUPANCY FAILED · LOCALIZATION NOT READY'; break;
      case 'unavailable': occupancyLabel = 'OCCUPANCY UNAVAILABLE · LOCALIZATION NOT READY'; break;
    }
  }
  return occupancyLabel;
}

function persistedScanStorageLabel(
  phase: ScanStoragePhase,
  objectId: string | null,
  persistence: ScanPersistenceState | null,
): string {
  const scanLabel = scanStorageLabel(phase, objectId);
  if (phase !== 'saved' && phase !== 'loaded') return scanLabel;
  return `${scanLabel} · ${occupancyStatusLabel(persistence)}`;
}

export default function App({ objectId, sessionId }: AppProps) {
  const [xrSessionActive, setXrSessionActive] = useState(false);
  const [xrCapabilities, setXrCapabilities] = useState<WebXrCapabilities | null>(null);
  const [computeProfile, setComputeProfile] = useState<DeviceComputeProfile | null>(null);
  const [scanSnapshot, setScanSnapshot] = useState<ScanPointCloudSnapshot | null>(null);
  const [scanStoragePhase, setScanStoragePhase] = useState<ScanStoragePhase>(
    objectId === null || sessionId === null ? 'unbound' : 'loading',
  );
  const [scanStorageError, setScanStorageError] = useState<string | null>(null);
  const [scanPersistence, setScanPersistence] = useState<ScanPersistenceState | null>(null);
  const [display, setDisplay] = useState<ScanDisplayOptions>(() => displayForScanMode('quality'));
  const [scanMode, setScanMode] = useState<ScanMode | null>('quality');
  const [viewCommand, setViewCommand] = useState<ViewCommand>({
    preset: 'perspective',
    sequence: 0,
  });
  const storageGeneration = useRef(0);
  const registrationForSession = useRef<{ objectId: string; sessionId: string; artifact: RegistrationArtifact } | null>(null);
  const onRegistrationChange = useCallback((artifact: RegistrationArtifact | null): void => {
    registrationForSession.current = artifact === null || objectId === null || sessionId === null
      ? null
      : { objectId, sessionId, artifact };
  }, [objectId, sessionId]);
  useEffect(() => { registrationForSession.current = null; }, [objectId, sessionId]);
  const scanIntent = useRef<ScanStartIntent>('fresh');
  const continuationBase = useRef<ScanPointCloudSnapshot | null>(null);
  const scanPersistenceController = useRef<ScanPersistenceController | null>(null);
  if (scanPersistenceController.current === null) {
    scanPersistenceController.current = new ScanPersistenceController({
      saveScan: saveObjectScanArtifact,
      saveOccupancy: saveSessionOccupancyArtifact,
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
    const generation = ++storageGeneration.current;
    persistenceController.reset();
    scanIntent.current = 'fresh';
    continuationBase.current = null;
    setScanSnapshot(null);
    setScanStorageError(null);

    if (objectId === null || sessionId === null) {
      setScanStoragePhase('unbound');
      return undefined;
    }

    setScanStoragePhase('loading');
    let cancelled = false;
    void loadObjectScanArtifact(objectId, sessionId).then(
      (artifact) => {
        if (cancelled || generation !== storageGeneration.current) return;
        if (artifact === null) {
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
          const message = reason instanceof Error ? reason.message : String(reason);
          setScanStorageError(message);
          setScanStoragePhase('error');
          clientDiagnostics.warn('SCAN_RESULT_RESTORE_FAILED', { objectId, sessionId, error: message });
          void clientDiagnostics.flush();
        }
      },
      (reason) => {
        if (cancelled || generation !== storageGeneration.current) return;
        const message = reason instanceof Error ? reason.message : String(reason);
        setScanStorageError(message);
        setScanStoragePhase('error');
        void persistenceController.reconcile(objectId, sessionId, null);
        clientDiagnostics.warn('SCAN_RESULT_RESTORE_FAILED', { objectId, sessionId, error: message });
        void clientDiagnostics.flush();
      },
    );

    return () => { cancelled = true; };
  }, [objectId, sessionId, persistenceController, selectMode]);

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
    ).then((state) => {
        if (generation !== storageGeneration.current) return;
        if (state?.scan !== 'saved') {
          const message = state?.scanError ?? 'The scan upload was not confirmed.';
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
    });
  }, [objectId, sessionId, persistenceController, selectMode]);

  const retryOccupancy = useCallback((): void => {
    if (objectId === null || sessionId === null) return;
    const generation = storageGeneration.current;
    void persistenceController.retry(objectId, sessionId).then((state) => {
      if (generation !== storageGeneration.current || state === null) return;
      const event = state.occupancy === 'ready' ? 'SCAN_OCCUPANCY_SAVED' : 'SCAN_OCCUPANCY_SAVE_FAILED';
      clientDiagnostics.info(event, { objectId, sessionId, error: state.occupancyError });
      void clientDiagnostics.flush();
    });
  }, [objectId, sessionId, persistenceController]);

  const deleteSavedScan = useCallback((): void => {
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
  }, [objectId, sessionId, persistenceController]);

  const hasMeasuredScan = scanSnapshot !== null && scanSnapshot.pointCount > 0;
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
  const storageBusy = scanStoragePhase === 'loading' || scanStoragePhase === 'saving' || scanStoragePhase === 'deleting';
  const localizationDataReady = scanPersistence?.scan === 'saved' && scanPersistence.occupancy === 'ready';

  return (
    <main className="app-shell">
      {!xrSessionActive && (
        <header className="app-header app-header-compact">
          <div>
            <div className="eyebrow">ESP32 GEOLOCATOR</div>
            <h1>3D search volume</h1>
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
              title={computeProfile === null ? 'Compute capability negotiation in progress' : computeProfileTitle(computeProfile)}
            >
              {computeProfile === null
                ? 'CPU …'
                : `CPU ${computeProfile.reportedLogicalProcessors} · W${computeProfile.liveWorkerBudget}`}
            </span>
            <span className={`badge ${hasMeasuredScan ? 'good' : 'warning'}`}>
              {hasMeasuredScan ? 'VOLUME READY' : 'READY'}
            </span>
            <span className={`badge ${storageGood && localizationDataReady ? 'good' : storageGood || scanStoragePhase === 'error' ? 'warning' : ''}`}>
              {persistedScanStorageLabel(scanStoragePhase, objectId, scanPersistence)}
            </span>
          </div>
        </header>
      )}

      {!xrSessionActive && scanStorageError !== null && (
        <div className="xr-runtime-error" role="alert">{scanStorageError}</div>
      )}

      {!xrSessionActive && hasMeasuredScan && (
        <section className="toolbar toolbar-compact scan-toolbar" aria-label="3D search-volume controls">
          <div className="scan-mode-controls" aria-label="Scan result modes">
            <button type="button" aria-pressed={scanMode === 'quality'} onClick={() => selectMode('quality')}>Quality map</button>
            <button type="button" aria-pressed={scanMode === 'surface'} onClick={() => selectMode('surface')}>Stable surface</button>
            <button type="button" aria-pressed={scanMode === 'search-volume'} onClick={() => selectMode('search-volume')}>Search volume</button>
            <button type="button" aria-pressed={scanMode === 'mesh'} onClick={() => selectMode('mesh')}>Surface mesh</button>
          </div>

          <div className="scan-quality-summary" aria-label="Quality band counts">
            <span><i className="coverage-dot coverage-low" />weak <strong>{qualityBands.weak.toLocaleString()}</strong></span>
            <span><i className="coverage-dot coverage-mid" />improving <strong>{qualityBands.improving.toLocaleString()}</strong></span>
            <span><i className="coverage-dot coverage-high" />stable <strong>{qualityBands.stable.toLocaleString()}</strong></span>
            <span>operator pins <strong>{scanSnapshot?.manualControlPoints.length.toLocaleString() ?? '0'}</strong></span>
            <span>edges <strong>{scanSnapshot?.operatorEdges.length.toLocaleString() ?? '0'}</strong></span>
          </div>

          <div className="scan-toolbar-group">
            <button type="button" onClick={() => requestView('perspective')}>3D</button>
            <button type="button" onClick={() => requestView('top')}>Top</button>
            <button type="button" onClick={() => requestView('front')}>Front</button>
          </div>
          <div className="scan-toolbar-group scan-layer-controls" aria-label="Reconstruction layers">
            <button type="button" aria-pressed={display.goodPoints} onClick={() => toggleDisplay('goodPoints')}>Stable surfels</button>
            <button type="button" aria-pressed={display.badPoints} onClick={() => toggleDisplay('badPoints')}>Tentative surfels</button>
            <button type="button" aria-pressed={display.mesh} onClick={() => toggleDisplay('mesh')}>Mesh</button>
            <button type="button" aria-pressed={display.freeAir} onClick={() => toggleDisplay('freeAir')}>Verified free</button>
            <button type="button" aria-pressed={display.unknownSpace} onClick={() => toggleDisplay('unknownSpace')}>Unknown candidate</button>
            <button type="button" aria-pressed={display.manualPoints} onClick={() => toggleDisplay('manualPoints')}>Operator geometry</button>
            <button type="button" aria-pressed={display.dimensions} onClick={() => toggleDisplay('dimensions')}>Dimensions</button>
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
          <ResultViewportErrorBoundary key={`${objectId ?? 'none'}-${scanSnapshot.depthFrameCount}-${scanSnapshot.pointCount}`}>
            <Suspense
              fallback={
                <div className="viewport-shell scan-viewport" role="status" aria-live="polite">
                  Loading 3D result renderer…
                </div>
              }
            >
              <ScanPointCloudViewport
                snapshot={scanSnapshot}
                viewCommand={viewCommand}
                display={display}
                mode={scanMode}
              />
            </Suspense>
          </ResultViewportErrorBoundary>

          <aside className="side-panel scan-result-panel">
            <section className="panel-card">
              <div className="panel-title">SEARCH VOLUME</div>
              <dl className="status-list scan-result-primary">
                <div><dt>SD persistence</dt><dd>{persistedScanStorageLabel(scanStoragePhase, objectId, scanPersistence)}</dd></div>
                <div><dt>Localization occupancy</dt><dd>{scanPersistence?.scan === 'saved' ? occupancyStatusLabel(scanPersistence) : 'OCCUPANCY STATUS CHECKING'}</dd></div>
                <div><dt>Stable boundary points</dt><dd>{scanSnapshot.pointCount.toLocaleString()}</dd></div>
                <div><dt>Tentative boundary points</dt><dd>{scanSnapshot.tentativePointCount.toLocaleString()}</dd></div>
                <div><dt>Manual corner pins</dt><dd>{scanSnapshot.manualControlPoints.length.toLocaleString()}</dd></div>
                <div><dt>Confirmed structural edges</dt><dd>{scanSnapshot.operatorEdges.length.toLocaleString()}</dd></div>
                <div><dt>Known free air voxels</dt><dd>{scanSnapshot.knownFreeVoxelCount.toLocaleString()}</dd></div>
                <div><dt>Observed surface voxels</dt><dd>{scanSnapshot.occupiedSurfaceVoxelCount.toLocaleString()}</dd></div>
                <div><dt>Unknown / candidate voxels</dt><dd>{scanSnapshot.unknownCandidateVoxelCount.toLocaleString()}</dd></div>
                {scanSnapshot.occupancyCellsSaturated === true && (
                  <div><dt>Occupancy map full</dt><dd>{(scanSnapshot.occupancyDroppedCellUpdates ?? 0).toLocaleString()} updates dropped — areas scanned late stay UNKNOWN</dd></div>
                )}
                {scanSnapshot.occupancyBridgeRescanComponentCount > 0 && (
                  <div><dt>Rescan bridge islands</dt><dd>{scanSnapshot.occupancyBridgeRescanComponentCount.toLocaleString()}</dd></div>
                )}
                <div><dt>Surface residual</dt><dd>{Math.round(scanSnapshot.averageStableResidualMeters * 1000)} mm</dd></div>
                {extent !== null && (
                  <>
                    <div><dt>Extent X</dt><dd>{formatMeters(extent[0])}</dd></div>
                    <div><dt>Extent Y</dt><dd>{formatMeters(extent[1])}</dd></div>
                    <div><dt>Height Z</dt><dd>{formatMeters(extent[2])}</dd></div>
                  </>
                )}
              </dl>

              {scanPersistence?.scan === 'saved' &&
                (scanPersistence.occupancy === 'failed' || scanPersistence.occupancy === 'unavailable') && (
                <p className="scan-volume-note" role="status">
                  {scanPersistence.occupancyError ?? 'Localization data is not ready until occupancy is saved.'}
                </p>
              )}

              {persistenceController.retryAvailable && (
                <button type="button" className="scan-clear-button" disabled={scanPersistence?.occupancy === 'saving'} onClick={retryOccupancy}>
                  Retry occupancy save
                </button>
              )}

              {scanSnapshot.occupancyBridgeRescanComponentCount > 0 && (
                <p className="scan-volume-note">
                  Weak measured surfels suggest a possible bridge from retained structure to {scanSnapshot.occupancyBridgeRescanComponentCount} quarantined island(s). Rescan those boundaries. They are not allowed to expand the UNKNOWN search envelope until their own physical support becomes sufficient.
                </p>
              )}

              <p className="scan-volume-note">
                Red and yellow points are still uncertain. Green surfels are the stable measured boundary. Blue voxels are ray-verified free air. Amber voxels are still unknown and therefore remain valid seismic-source search volume. Magenta pins and lines are operator-confirmed structural constraints.
              </p>

              <details className="xr-technical-details scan-result-details">
                <summary>Technical details</summary>
                <dl className="status-list">
                  <div><dt>Retained surface hypotheses</dt><dd>{scanSnapshot.rawPointCount.toLocaleString()}</dd></div>
                  <div><dt>Promoted surfels</dt><dd>{scanSnapshot.promotedPointCount.toLocaleString()}</dd></div>
                  <div><dt>UNKNOWN envelope components</dt><dd>{scanSnapshot.occupancyEnvelopeComponentCount.toLocaleString()}</dd></div>
                  <div><dt>Quarantined surface components</dt><dd>{scanSnapshot.occupancyQuarantinedSurfaceComponentCount.toLocaleString()}</dd></div>
                  <div><dt>Quarantined surface points</dt><dd>{scanSnapshot.occupancyQuarantinedSurfacePointCount.toLocaleString()}</dd></div>
                  <div><dt>Weak-bridge rescan components</dt><dd>{scanSnapshot.occupancyBridgeRescanComponentCount.toLocaleString()}</dd></div>
                  <div><dt>Weak-bridge diagnostic</dt><dd>{scanSnapshot.occupancyBridgeEvidenceTruncated ? 'bounded / incomplete' : 'complete'}</dd></div>
                  <div><dt>Ready coverage cells</dt><dd>{scanSnapshot.readyCoverageCellCount.toLocaleString()}</dd></div>
                  <div><dt>Independent keyframes</dt><dd>{scanSnapshot.integrationKeyframes.toLocaleString()}</dd></div>
                  <div><dt>Coverage cells</dt><dd>{scanSnapshot.coverageCellCount.toLocaleString()}</dd></div>
                  <div><dt>Coverage cell</dt><dd>{Math.round(scanSnapshot.coverageCellSizeMeters * 1000)} mm</dd></div>
                  <div><dt>Surface association voxel</dt><dd>{Math.round(scanSnapshot.voxelSizeMeters * 1000)} mm</dd></div>
                  <div><dt>Occupancy display voxel</dt><dd>{Math.round(scanSnapshot.occupancyVoxelSizeMeters * 1000)} mm</dd></div>
                  <div><dt>Adaptive reps / cell</dt><dd>{scanSnapshot.adaptivePointsPerCell}</dd></div>
                  <div><dt>Polish frames</dt><dd>{scanSnapshot.polishFrames.toLocaleString()}</dd></div>
                  <div><dt>Residual rejects</dt><dd>{scanSnapshot.residualRejectedSamples.toLocaleString()}</dd></div>
                  <div><dt>Stale tentative pruned</dt><dd>{scanSnapshot.staleTentativePruned.toLocaleString()}</dd></div>
                  <div><dt>Candidate samples</dt><dd>{scanSnapshot.candidateSampleCount.toLocaleString()}</dd></div>
                  <div><dt>Decimated / rejected</dt><dd>{scanSnapshot.discardedSampleCount.toLocaleString()}</dd></div>
                  <div><dt>In-cell replacements</dt><dd>{scanSnapshot.replacementCount.toLocaleString()}</dd></div>
                  <div><dt>Memory budget</dt><dd>{scanSnapshot.memoryBudgetPoints.toLocaleString()}</dd></div>
                  <div><dt>Coverage rejects</dt><dd>{scanSnapshot.coverageRejectedCount.toLocaleString()}</dd></div>
                  <div><dt>Depth frames</dt><dd>{scanSnapshot.depthFrameCount.toLocaleString()}</dd></div>
                  <div><dt>Integrated frames</dt><dd>{scanSnapshot.sampledDepthFrames.toLocaleString()}</dd></div>
                  <div><dt>Depth cap</dt><dd>{formatMeters(scanSnapshot.maxDepthMeters)}</dd></div>
                </dl>
              </details>

              <button type="button" className="scan-clear-button" disabled={storageBusy} onClick={deleteSavedScan}>
                {scanStoragePhase === 'deleting' ? 'Deleting…' : 'Delete saved scan'}
              </button>
            </section>
          </aside>
        </section>
      )}
    </main>
  );
}
