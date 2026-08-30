import { useState } from 'react';

import { SceneViewport, type ViewCommand, type ViewPreset } from './components/SceneViewport';
import { SensorPanel } from './components/SensorPanel';
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
          <h1>Sensor placement & scan geometry</h1>
          <p>
            Hardware-independent viewer for MAIN + GEO1…GEO4 in one metric coordinate frame.
          </p>
        </div>
        <div className="header-badges">
          <span className="badge good">OFFLINE ASSETS</span>
          <span className="badge warning">DEMO SCAN</span>
        </div>
      </header>

      <section className="toolbar" aria-label="3D view controls">
        <button type="button" onClick={() => requestView('perspective')}>Perspective</button>
        <button type="button" onClick={() => requestView('top')}>Top</button>
        <button type="button" onClick={() => requestView('front')}>Front</button>
        <span className="toolbar-note">Tap a marker or device row to select it.</span>
      </section>

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
        Next data sources: scanned triangle mesh → AprilTag poses → locked sensor XYZ → barometer / IMU / ranging fusion.
      </footer>
    </main>
  );
}
