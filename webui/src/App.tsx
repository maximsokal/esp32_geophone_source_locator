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

  return (
    <main className="app-shell">
      <header className="app-header">
        <div>
          <div className="eyebrow">ESP32 GEOLOCATOR · 3D WORKSPACE</div>
          <h1>Metric browser 3D scanning & sensor placement</h1>
          <p>
            Android WebXR/ARCore depth is processed locally into a filtered metric point cloud. The ESP32 only serves the trusted offline web application.
          </p>
        </div>
        <div className="header-badges">
          <span className="badge good">TRUSTED HTTPS</span>
          <span className="badge good">WEBXR DEPTH</span>
          <span className={`badge ${hasMeasuredScan ? 'good' : 'warning'}`}>
            {hasMeasuredScan ? 'REAL 3D SCAN' : 'READY TO SCAN'}
          </span>
          {scanSnapshot?.capacityReached && (
            <span className="badge warning">CLOUD LIMIT REACHED</span>
          )}
        </div>
      </header>

      <section className="toolbar" aria-label="3D view controls">
        <button type="button" disabled={!hasMeasuredScan} onClick={() => requestView('perspective')}>Perspective</button>
        <button type="button" disabled={!hasMeasuredScan} onClick={() => requestView('top')}>Top</button>
        <button type="button" disabled={!hasMeasuredScan} onClick={() => requestView('front')}>Front</button>
        <span className="toolbar-note">
          {hasMeasuredScan
            ? 'Filtered measured cloud: drag to orbit. Desktop wheel zoom is available.'
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
                    <dt>Raw unique voxels</dt>
                    <dd>{scanSnapshot.rawPointCount.toLocaleString()}</dd>
                  </div>
                  <div>
                    <dt>Cloud capacity</dt>
                    <dd>
                      {scanSnapshot.capacityReached
                        ? `LIMIT ${scanSnapshot.maxPointCapacity.toLocaleString()}`
                        : `${scanSnapshot.rawPointCount.toLocaleString()} / ${scanSnapshot.maxPointCapacity.toLocaleString()}`}
                    </dd>
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
                    <dt>Voxel resolution</dt>
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

              {scanSnapshot.capacityReached && (
                <section className="panel-card">
                  <div className="panel-title">Cloud capacity reached</div>
                  <p className="scan-result-copy">
                    This scan filled the raw voxel budget before the AR session ended, so later viewpoints could not add new geometry. The current build raises the budget to {scanSnapshot.maxPointCapacity.toLocaleString()} voxels; if this warning appears again, the next step is adaptive decimation instead of simply raising memory use again.
                  </p>
                </section>
              )}

              <section className="panel-card grow">
                <div className="panel-title">What this result is</div>
                <p className="scan-result-copy">
                  These are real metric surface samples reconstructed from ARCore depth maps plus the depth-sensor pose for each sampled XR frame. A local depth-neighbour filter rejects unstable pixels and repeated observations are used to remove isolated one-frame points.
                </p>
                <p className="scan-result-copy">
                  The displayed room/object extents are now robust 1st–99th percentile bounds per axis, while the raw min/max span is kept as a diagnostic. This prevents a small number of depth spikes from pretending that the room is several metres taller than it really is.
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
                For a room, a 360° turn from one position is enough to validate depth geometry. For a rubble/object scan, also move the phone between viewpoints so occluded surfaces can enter the measured shell.
              </p>
            </div>
          </section>
        )
      )}

      <footer className="app-footer">
        Current pipeline: trusted HTTPS → immersive WebXR → ARCore CPU depth → local depth consistency filter → metric XYZ reconstruction → 40 mm voxel cloud → repeated-observation cleanup → robust bounds → local 3D review.
      </footer>
    </main>
  );
}
