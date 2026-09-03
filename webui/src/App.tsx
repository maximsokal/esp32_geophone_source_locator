import { Component, lazy, Suspense, useCallback, useEffect, useState, type ErrorInfo, type ReactNode } from 'react';

import './scan.css';

import type { ViewCommand, ViewPreset } from './components/SceneViewport';
import { XrRuntimePanel } from './components/XrRuntimePanel';
import { clientDiagnostics } from './data/clientDiagnostics';
import { countCoverageQualityBands } from './xr/coverageQuality';
import {
  deviceComputeProfilePromise,
  type DeviceComputeProfile,
} from './xr/deviceComputeProfile';
import {
  displayForScanMode,
  type ScanDisplayOptions,
  type ScanMode,
} from './xr/scanDisplayModes';
import type { ScanPointCloudSnapshot, WebXrCapabilities } from './xr/webxrRuntime';

const ScanPointCloudViewport = lazy(async () => {
  const module = await import('./components/ScanPointCloudViewport');
  return { default: module.ScanPointCloudViewport };
});

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
          3D result renderer could not be loaded. Reload the page to retry. ({this.state.message})
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

export default function App() {
  const [xrSessionActive, setXrSessionActive] = useState(false);
  const [xrCapabilities, setXrCapabilities] = useState<WebXrCapabilities | null>(null);
  const [computeProfile, setComputeProfile] = useState<DeviceComputeProfile | null>(null);
  const [scanSnapshot, setScanSnapshot] = useState<ScanPointCloudSnapshot | null>(null);
  const [display, setDisplay] = useState<ScanDisplayOptions>(() => displayForScanMode('quality'));
  const [scanMode, setScanMode] = useState<ScanMode | null>('quality');
  const [viewCommand, setViewCommand] = useState<ViewCommand>({
    preset: 'perspective',
    sequence: 0,
  });

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
    const next = displayForScanMode(mode);
    setScanMode(mode);
    setDisplay(next);
    clientDiagnostics.info('SCAN_RESULT_MODE', { mode, display: next });
  }, []);

  const toggleDisplay = useCallback((key: keyof ScanDisplayOptions): void => {
    setScanMode(null);
    setDisplay((current) => {
      const next = { ...current, [key]: !current[key] };
      clientDiagnostics.info('SCAN_RESULT_LAYER_TOGGLE', { layer: key, enabled: next[key] });
      return next;
    });
  }, []);

  const handleScanReady = useCallback((snapshot: ScanPointCloudSnapshot | null): void => {
    setScanSnapshot(snapshot);
    if (snapshot !== null) {
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
    }
  }, [selectMode]);

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
          </div>
        </header>
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
        onSessionActiveChange={setXrSessionActive}
        onPointCloudReady={handleScanReady}
        onCapabilitiesChange={setXrCapabilities}
      />

      {!xrSessionActive && hasMeasuredScan && scanSnapshot !== null && (
        <section className="workspace">
          <ResultViewportErrorBoundary>
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
                <div><dt>Stable boundary points</dt><dd>{scanSnapshot.pointCount.toLocaleString()}</dd></div>
                <div><dt>Tentative boundary points</dt><dd>{scanSnapshot.tentativePointCount.toLocaleString()}</dd></div>
                <div><dt>Manual corner pins</dt><dd>{scanSnapshot.manualControlPoints.length.toLocaleString()}</dd></div>
                <div><dt>Confirmed structural edges</dt><dd>{scanSnapshot.operatorEdges.length.toLocaleString()}</dd></div>
                <div><dt>Known free air voxels</dt><dd>{scanSnapshot.knownFreeVoxelCount.toLocaleString()}</dd></div>
                <div><dt>Observed surface voxels</dt><dd>{scanSnapshot.occupiedSurfaceVoxelCount.toLocaleString()}</dd></div>
                <div><dt>Unknown / candidate voxels</dt><dd>{scanSnapshot.unknownCandidateVoxelCount.toLocaleString()}</dd></div>
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

              <button type="button" className="scan-clear-button" onClick={() => setScanSnapshot(null)}>
                Clear scan
              </button>
            </section>
          </aside>
        </section>
      )}
    </main>
  );
}
