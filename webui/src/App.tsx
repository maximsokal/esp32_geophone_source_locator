import { useState } from 'react';

import { SceneViewport, type ViewCommand, type ViewPreset } from './components/SceneViewport';
import { SensorPanel } from './components/SensorPanel';
import { XrRuntimePanel } from './components/XrRuntimePanel';
import { demoScene } from './data/demoScene';

export default function App() {
  const [selectedId, setSelectedId] = useState('MAIN');
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

  return (
    <main className="app-shell">
      <header className="app-header">
        <div>
          <div className="eyebrow">ESP32 GEOLOCATOR · 3D WORKSPACE</div>
          <h1>Sensor placement & browser AR tracking</h1>
          <p>
            Browser-only metric workspace for MAIN + GEO1…GEO4, with a real WebXR immersive-AR runtime on the phone.
          </p>
        </div>
        <div className="header-badges">
          <span className="badge good">TRUSTED HTTPS</span>
          <span className="badge good">WEBXR-A</span>
          <span className="badge warning">DEMO SCAN</span>
        </div>
      </header>

      <section className="toolbar" aria-label="3D view controls">
        <button type="button" onClick={() => requestView('perspective')}>Perspective</button>
        <button type="button" onClick={() => requestView('top')}>Top</button>
        <button type="button" onClick={() => requestView('front')}>Front</button>
        <span className="toolbar-note">Tap a marker or device row to select it.</span>
      </section>

      <XrRuntimePanel />

      <section className="workspace">
        <SceneViewport
          scene={demoScene}
          selectedId={selectedId}
          viewCommand={viewCommand}
          onSelect={setSelectedId}
        />
        <SensorPanel scene={demoScene} selectedId={selectedId} onSelect={setSelectedId} />
      </section>

      <footer className="app-footer">
        WEB-XR-A tracks the phone pose only. Next: ARCore/WebXR depth sensing → point cloud / surface samples → mesh reconstruction → AprilTag sensor registration.
      </footer>
    </main>
  );
}
