import { useState } from 'react';

import './scan.css';

import { ScanPointCloudViewport } from './components/ScanPointCloudViewport';
import type { ViewCommand, ViewPreset } from './components/SceneViewport';
import { XrRuntimePanel } from './components/XrRuntimePanel';
import type { ScanPointCloudSnapshot } from './xr/webxrRuntime';

function formatMeters(value: number): string {
  return `${value.toFixed(2)} m`;
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
  const bounds = scanSnapshot?.bounds ?? null;
  const extent =
    bounds === null
      ? null
      : [
          bounds.max[0] - bounds.min[0],
          bounds.max[1] - bounds.min[1],
          bounds.max[2] - bounds.min[2],
        ] as const;

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
                  {extent !== null && (
                    <>
                      <div>
                        <dt>Extent X</dt>
                        <dd>{formatMeters(extent[0])}</dd>
                      </div>
                      <div>
                        <dt>Extent Y</dt>
                        <dd>{formatMeters(extent[1])}</dd>
                      </div>
                      <div>
                        <dt>Extent Z / height</dt>
                        <dd>{formatMeters(extent[2])}</dd>
                      </div>
                    </>
                  )}
                </dl>
              </section>

              <section className="panel-card grow">
                <div className="panel-title">What this result is</div>
                <p className="scan-result-copy">
                  These are real metric surface samples reconstructed from ARCore depth maps plus the depth-sensor pose for each sampled XR frame. A local depth-neighbour filter rejects unstable pixels and repeated observations are used to remove isolated one-frame points.
                </p>
                <p className="scan-result-copy">
                  This validation build caps accepted depth at 5 m. Once the measured cloud is geometrically sane, the next layer will add selectable scan range, surface/occupancy reconstruction and TAG0 MAIN + TAG1…TAG4 registration.
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
                For this validation pass keep the target mostly within about 0.5–4 m of the phone. During a valid scan the overlay must show <b>DEPTH ACTIVE</b> with increasing depth frames and 3D points.
              </p>
            </div>
          </section>
        )
      )}

      <footer className="app-footer">
        Current pipeline: trusted HTTPS → immersive WebXR → ARCore CPU depth → local depth consistency filter → metric XYZ reconstruction → 40 mm voxel cloud → repeated-observation cleanup → local 3D review.
      </footer>
    </main>
  );
}
