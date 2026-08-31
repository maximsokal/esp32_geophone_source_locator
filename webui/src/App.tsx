import { useState } from 'react';

import './scan.css';

import { ScanPointCloudViewport } from './components/ScanPointCloudViewport';
import type { ViewCommand, ViewPreset } from './components/SceneViewport';
import { XrRuntimePanel } from './components/XrRuntimePanel';
import type { ScanPointCloudSnapshot } from './xr/webxrRuntime';

function formatMeters(value: number): string {
  return `${value.toFixed(2)} m`;
}

function extentFromBounds(
  bounds: ScanPointCloudSnapshot['bounds'],
): readonly [number, number, number] | null {
  if (bounds === null) {
    return null;
  }

  return [
    bounds.max[0] - bounds.min[0],
    bounds.max[1] - bounds.min[1],
    bounds.max[2] - bounds.min[2],
  ];
}

export default function App() {
  const [xrSessionActive, setXrSessionActive] = useState(false);
  const [scanSnapshot, setScanSnapshot] = useState<ScanPointCloudSnapshot | null>(null);
  const [viewCommand, setViewCommand] = useState<ViewCommand>({
    preset: 'perspective',
    sequence: 0,
  });

  const requestView = (preset: ViewPreset) => {
    setViewCommand((current) => ({
      preset,
      sequence: current.sequence + 1,
    }));
  };

  const hasMeasuredScan = scanSnapshot !== null && scanSnapshot.pointCount > 0;
  const extent = extentFromBounds(scanSnapshot?.bounds ?? null);
  const fullExtent = extentFromBounds(scanSnapshot?.fullBounds ?? null);
  const adaptiveDecimationUsed =
    scanSnapshot !== null &&
    (scanSnapshot.replacementCount > 0 || scanSnapshot.rebalanceEvictionCount > 0);

  return (
    <main className="app-shell">
      <header className="app-header">
        <div>
          <div className="eyebrow">ESP32 GEOLOCATOR · 3D WORKSPACE</div>
          <h1>Metric browser 3D scanning & sensor placement</h1>
          <p>
            Android WebXR/ARCore depth is processed locally into a bounded-memory, spatially balanced metric point cloud. The ESP32 only serves the trusted offline web application.
          </p>
        </div>
        <div className="header-badges">
          <span className="badge good">TRUSTED HTTPS</span>
          <span className="badge good">WEBXR DEPTH</span>
          <span className={`badge ${hasMeasuredScan ? 'good' : 'warning'}`}>
            {hasMeasuredScan ? 'REAL 3D SCAN' : 'READY TO SCAN'}
          </span>
          {adaptiveDecimationUsed && (
            <span className="badge good">ADAPTIVE DECIMATION</span>
          )}
          {(scanSnapshot?.coverageRejectedCount ?? 0) > 0 && (
            <span className="badge warning">COVERAGE BUDGET EXHAUSTED</span>
          )}
        </div>
      </header>

      <section className="toolbar" aria-label="3D view controls">
        <button type="button" disabled={!hasMeasuredScan} onClick={() => requestView('perspective')}>Perspective</button>
        <button type="button" disabled={!hasMeasuredScan} onClick={() => requestView('top')}>Top</button>
        <button type="button" disabled={!hasMeasuredScan} onClick={() => requestView('front')}>Front</button>
        <span className="toolbar-note">
          {hasMeasuredScan
            ? 'Spatially balanced measured cloud: drag to orbit. Desktop wheel zoom is available.'
            : 'Start a real 3D scan below. No demo mesh is shown.'}
        </span>
      </section>

      <XrRuntimePanel
        onSessionActiveChange={setXrSessionActive}
        onPointCloudReady={setScanSnapshot}
      />

      {!xrSessionActive && (
        hasMeasuredScan && scanSnapshot !== null ? (
          <section className="workspace">
            <ScanPointCloudViewport snapshot={scanSnapshot} viewCommand={viewCommand} />

            <aside className="side-panel scan-result-panel">
              <section className="panel-card">
                <div className="panel-title">Measured scan result</div>
                <dl className="status-list">
                  <div>
                    <dt>Stable 3D points</dt>
                    <dd>{scanSnapshot.pointCount.toLocaleString()}</dd>
                  </div>
                  <div>
                    <dt>Retained representatives</dt>
                    <dd>{scanSnapshot.rawPointCount.toLocaleString()}</dd>
                  </div>
                  <div>
                    <dt>Spatial memory budget</dt>
                    <dd>{scanSnapshot.memoryBudgetPoints.toLocaleString()}</dd>
                  </div>
                  <div>
                    <dt>Coverage cells</dt>
                    <dd>{scanSnapshot.coverageCellCount.toLocaleString()}</dd>
                  </div>
                  <div>
                    <dt>Coverage cell size</dt>
                    <dd>{Math.round(scanSnapshot.coverageCellSizeMeters * 1000)} mm</dd>
                  </div>
                  <div>
                    <dt>Adaptive reps / cell</dt>
                    <dd>{scanSnapshot.adaptivePointsPerCell}</dd>
                  </div>
                  <div>
                    <dt>Candidate depth samples</dt>
                    <dd>{scanSnapshot.candidateSampleCount.toLocaleString()}</dd>
                  </div>
                  <div>
                    <dt>Samples decimated</dt>
                    <dd>{scanSnapshot.discardedSampleCount.toLocaleString()}</dd>
                  </div>
                  <div>
                    <dt>In-cell replacements</dt>
                    <dd>{scanSnapshot.replacementCount.toLocaleString()}</dd>
                  </div>
                  <div>
                    <dt>Adaptive rebalances</dt>
                    <dd>{scanSnapshot.rebalanceEvictionCount.toLocaleString()} evictions</dd>
                  </div>
                  <div>
                    <dt>Coverage rejects</dt>
                    <dd>{scanSnapshot.coverageRejectedCount.toLocaleString()}</dd>
                  </div>
                  <div>
                    <dt>Depth frames received</dt>
                    <dd>{scanSnapshot.depthFrameCount.toLocaleString()}</dd>
                  </div>
                  <div>
                    <dt>Frames sampled</dt>
                    <dd>{scanSnapshot.sampledDepthFrames.toLocaleString()}</dd>
                  </div>
                  <div>
                    <dt>Fine voxel resolution</dt>
                    <dd>{Math.round(scanSnapshot.voxelSizeMeters * 1000)} mm</dd>
                  </div>
                  <div>
                    <dt>Depth range cap</dt>
                    <dd>{formatMeters(scanSnapshot.maxDepthMeters)}</dd>
                  </div>
                  <div>
                    <dt>Bounds filter</dt>
                    <dd>
                      {scanSnapshot.boundsTrimFraction > 0
                        ? `${Math.round(scanSnapshot.boundsTrimFraction * 100)}% tails / axis`
                        : 'raw min/max'}
                    </dd>
                  </div>
                  {extent !== null && (
                    <>
                      <div>
                        <dt>Robust extent X</dt>
                        <dd>{formatMeters(extent[0])}</dd>
                      </div>
                      <div>
                        <dt>Robust extent Y</dt>
                        <dd>{formatMeters(extent[1])}</dd>
                      </div>
                      <div>
                        <dt>Robust extent Z / height</dt>
                        <dd>{formatMeters(extent[2])}</dd>
                      </div>
                    </>
                  )}
                  {fullExtent !== null && scanSnapshot.boundsTrimFraction > 0 && (
                    <div>
                      <dt>Raw outlier span XYZ</dt>
                      <dd>
                        {formatMeters(fullExtent[0])} / {formatMeters(fullExtent[1])} / {formatMeters(fullExtent[2])}
                      </dd>
                    </div>
                  )}
                </dl>
              </section>

              <section className="panel-card">
                <div className="panel-title">Adaptive memory policy</div>
                <p className="scan-result-copy">
                  The scanner no longer stores every unique 40 mm voxel until a hard limit is hit. Space is divided into {Math.round(scanSnapshot.coverageCellSizeMeters * 100)} cm coverage cells and the fixed {scanSnapshot.memoryBudgetPoints.toLocaleString()}-point budget is redistributed as new space appears.
                </p>
                <p className="scan-result-copy">
                  Each coverage cell keeps a deterministic, stability-weighted representative subset. Repeated observations refine existing representatives; dense already-covered regions are decimated or rebalanced so later viewpoints can still contribute new geometry.
                </p>
                {(scanSnapshot.coverageRejectedCount > 0) && (
                  <p className="scan-result-copy">
                    Coverage rejects are non-zero, meaning the scan discovered more coverage cells than the one-point-per-cell lower bound can represent. That is the only true spatial-budget exhaustion condition.
                  </p>
                )}
              </section>

              <section className="panel-card grow">
                <div className="panel-title">What this result is</div>
                <p className="scan-result-copy">
                  These are real metric surface samples reconstructed from ARCore depth maps plus the depth-sensor pose for each sampled XR frame. A local depth-neighbour filter rejects unstable depth pixels before reconstruction.
                </p>
                <p className="scan-result-copy">
                  Final stability filtering uses repeat observations and local spatial support, while displayed room/object extents use robust 1st–99th percentile bounds per axis. Raw min/max remains visible only as an outlier diagnostic.
                </p>
                <p className="scan-result-copy">
                  Rotating from one position can measure the surrounding visible shell, but it cannot reveal surfaces hidden behind furniture or rubble. For the diploma workflow the operator should translate the phone between viewpoints as well as rotate it before surface/occupancy reconstruction.
                </p>
                <button
                  type="button"
                  className="scan-clear-button"
                  onClick={() => setScanSnapshot(null)}
                >
                  Clear measured scan
                </button>
              </section>
            </aside>
          </section>
        ) : (
          <section className="scan-empty-state" aria-label="No measured 3D scan yet">
            <div className="scan-empty-icon">3D</div>
            <div>
              <h2>No measured scan yet</h2>
              <p>
                Press <b>Start 3D scan</b>, move slowly around the visible rubble/object while the live counter grows, then press <b>Finish scan & view 3D</b>.
              </p>
              <p>
                The live point count is now a bounded adaptive representative count. It may plateau or even decrease slightly when new spatial coverage causes a rebalance; that is expected and does not mean scanning stopped.
              </p>
            </div>
          </section>
        )
      )}

      <footer className="app-footer">
        Current pipeline: trusted HTTPS → immersive WebXR → ARCore CPU depth → local depth consistency → metric XYZ → 40 mm fine voxels → adaptive 300 mm coverage budgeting → stability filter → robust bounds → local 3D review.
      </footer>
    </main>
  );
}
