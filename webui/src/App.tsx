import { useState } from 'react';

import './scan.css';

import { ScanPointCloudViewport } from './components/ScanPointCloudViewport';
import { SceneViewport, type ViewCommand, type ViewPreset } from './components/SceneViewport';
import { SensorPanel } from './components/SensorPanel';
import { XrRuntimePanel } from './components/XrRuntimePanel';
import { demoScene } from './data/demoScene';
import type { ScanPointCloudSnapshot } from './xr/webxrRuntime';

function formatMeters(value: number): string {
  return `${value.toFixed(2)} m`;
}

export default function App() {
  const [selectedId, setSelectedId] = useState('MAIN');
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
            Android WebXR/ARCore depth is processed locally into a metric point cloud. The ESP32 only serves the trusted offline web application.
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
        <button type="button" onClick={() => requestView('perspective')}>Perspective</button>
        <button type="button" onClick={() => requestView('top')}>Top</button>
        <button type="button" onClick={() => requestView('front')}>Front</button>
        <span className="toolbar-note">
          {hasMeasuredScan
            ? 'Measured point cloud: drag to orbit, wheel/pinch to zoom.'
            : 'Start 3D scan above. The scene below is only the reference sensor-layout model.'}
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
                    <dt>3D points</dt>
                    <dd>{scanSnapshot.pointCount.toLocaleString()}</dd>
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
                  These are real metric surface samples reconstructed from ARCore depth maps plus the camera pose for each sampled XR frame. They are not the old demo mesh and are not uploaded to a cloud service.
                </p>
                <p className="scan-result-copy">
                  The next reconstruction layer will turn these surface samples into a decimated surface/occupancy representation and attach TAG0 MAIN + TAG1…TAG4 sensor coordinates to the same frame.
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
          <section className="workspace">
            <SceneViewport
              scene={demoScene}
              selectedId={selectedId}
              viewCommand={viewCommand}
              onSelect={setSelectedId}
            />
            <SensorPanel scene={demoScene} selectedId={selectedId} onSelect={setSelectedId} />
          </section>
        )
      )}

      <footer className="app-footer">
        Current pipeline: trusted HTTPS → immersive WebXR → ARCore CPU depth → metric XYZ reconstruction → 40 mm voxel point cloud → local 3D review. Next: surface/occupancy reconstruction and AprilTag registration.
      </footer>
    </main>
  );
}
