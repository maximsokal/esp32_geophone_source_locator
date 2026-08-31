import { useState } from 'react';

import './scan.css';

import {
  ScanPointCloudViewport,
  type ScanDisplayOptions,
} from './components/ScanPointCloudViewport';
import type { ViewCommand, ViewPreset } from './components/SceneViewport';
import { XrRuntimePanel } from './components/XrRuntimePanel';
import type { ScanPointCloudSnapshot } from './xr/webxrRuntime';

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

const initialDisplay: ScanDisplayOptions = {
  goodPoints: true,
  badPoints: true,
  dimensions: true,
  mesh: false,
  freeAir: false,
  unknownSpace: false,
};

export default function App() {
  const [xrSessionActive, setXrSessionActive] = useState(false);
  const [scanSnapshot, setScanSnapshot] = useState<ScanPointCloudSnapshot | null>(null);
  const [display, setDisplay] = useState<ScanDisplayOptions>(initialDisplay);
  const [viewCommand, setViewCommand] = useState<ViewCommand>({
    preset: 'perspective',
    sequence: 0,
  });

  const requestView = (preset: ViewPreset) => {
    setViewCommand((current) => ({ preset, sequence: current.sequence + 1 }));
  };

  const toggleDisplay = (key: keyof ScanDisplayOptions) => {
    setDisplay((current) => ({ ...current, [key]: !current[key] }));
  };

  const hasMeasuredScan = scanSnapshot !== null && scanSnapshot.pointCount > 0;
  const extent = extentFromBounds(scanSnapshot?.bounds ?? null);

  return (
    <main className="app-shell">
      {!xrSessionActive && (
        <header className="app-header app-header-compact">
          <div>
            <div className="eyebrow">ESP32 GEOLOCATOR</div>
            <h1>3D search volume</h1>
          </div>
          <div className="header-badges">
            <span className="badge good">HTTPS</span>
            <span className="badge good">WEBXR</span>
            <span className={`badge ${hasMeasuredScan ? 'good' : 'warning'}`}>
              {hasMeasuredScan ? 'VOLUME READY' : 'READY'}
            </span>
          </div>
        </header>
      )}

      {!xrSessionActive && hasMeasuredScan && (
        <section className="toolbar toolbar-compact scan-toolbar" aria-label="3D search-volume controls">
          <div className="scan-toolbar-group">
            <button type="button" onClick={() => requestView('perspective')}>3D</button>
            <button type="button" onClick={() => requestView('top')}>Top</button>
            <button type="button" onClick={() => requestView('front')}>Front</button>
          </div>
          <div className="scan-toolbar-group scan-layer-controls" aria-label="Reconstruction layers">
            <button type="button" aria-pressed={display.goodPoints} onClick={() => toggleDisplay('goodPoints')}>Surface</button>
            <button type="button" aria-pressed={display.badPoints} onClick={() => toggleDisplay('badPoints')}>Tentative</button>
            <button type="button" aria-pressed={display.mesh} onClick={() => toggleDisplay('mesh')}>Surface mesh</button>
            <button type="button" aria-pressed={display.freeAir} onClick={() => toggleDisplay('freeAir')}>Free air</button>
            <button type="button" aria-pressed={display.unknownSpace} onClick={() => toggleDisplay('unknownSpace')}>Unknown</button>
            <button type="button" aria-pressed={display.dimensions} onClick={() => toggleDisplay('dimensions')}>Dimensions</button>
          </div>
        </section>
      )}

      <XrRuntimePanel
        onSessionActiveChange={setXrSessionActive}
        onPointCloudReady={setScanSnapshot}
      />

      {!xrSessionActive && hasMeasuredScan && scanSnapshot !== null && (
        <section className="workspace">
          <ScanPointCloudViewport
            snapshot={scanSnapshot}
            viewCommand={viewCommand}
            display={display}
          />

          <aside className="side-panel scan-result-panel">
            <section className="panel-card">
              <div className="panel-title">SEARCH VOLUME</div>
              <dl className="status-list scan-result-primary">
                <div><dt>Stable boundary points</dt><dd>{scanSnapshot.pointCount.toLocaleString()}</dd></div>
                <div><dt>Known free air voxels</dt><dd>{scanSnapshot.knownFreeVoxelCount.toLocaleString()}</dd></div>
                <div><dt>Observed surface voxels</dt><dd>{scanSnapshot.occupiedSurfaceVoxelCount.toLocaleString()}</dd></div>
                <div><dt>Unknown / candidate voxels</dt><dd>{scanSnapshot.unknownCandidateVoxelCount.toLocaleString()}</dd></div>
                <div><dt>Tentative boundary points</dt><dd>{scanSnapshot.tentativePointCount.toLocaleString()}</dd></div>
                <div><dt>Surface residual</dt><dd>{Math.round(scanSnapshot.averageStableResidualMeters * 1000)} mm</dd></div>
                {extent !== null && (
                  <>
                    <div><dt>Extent X</dt><dd>{formatMeters(extent[0])}</dd></div>
                    <div><dt>Extent Y</dt><dd>{formatMeters(extent[1])}</dd></div>
                    <div><dt>Height Z</dt><dd>{formatMeters(extent[2])}</dd></div>
                  </>
                )}
              </dl>

              <p className="scan-volume-note">
                Free air is ray-verified and can be excluded from seismic source search. Unknown volume is deliberately retained as a candidate because the camera cannot see inside rubble.
              </p>

              <details className="xr-technical-details scan-result-details">
                <summary>Technical details</summary>
                <dl className="status-list">
                  <div><dt>Retained surface hypotheses</dt><dd>{scanSnapshot.rawPointCount.toLocaleString()}</dd></div>
                  <div><dt>Promoted surfels</dt><dd>{scanSnapshot.promotedPointCount.toLocaleString()}</dd></div>
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
