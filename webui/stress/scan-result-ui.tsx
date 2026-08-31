import { useState } from 'react';
import { createRoot } from 'react-dom/client';

import '../src/styles.css';
import '../src/scan.css';
import { ScanPointCloudViewport } from '../src/components/ScanPointCloudViewport';
import type { ViewCommand, ViewPreset } from '../src/components/SceneViewport';
import {
  displayForScanMode,
  type ScanDisplayOptions,
  type ScanMode,
} from '../src/xr/scanDisplayModes';
import type { ScanPointCloudSnapshot } from '../src/xr/webxrRuntime';

const stablePositions = new Float32Array([
  -1.0, -0.8, 0.0,  -0.2, -0.8, 0.0,  0.6, -0.8, 0.0,
  -1.0,  0.0, 0.2,  -0.2,  0.0, 0.4,  0.6,  0.0, 0.2,
  -1.0,  0.8, 0.0,  -0.2,  0.8, 0.2,  0.6,  0.8, 0.0,
]);
const normals = new Float32Array(stablePositions.length);
for (let i = 2; i < normals.length; i += 3) normals[i] = 1;

const snapshot = {
  positions: stablePositions,
  normals,
  confidence: new Float32Array([0.25, 0.52, 0.92, 0.36, 0.66, 0.95, 0.40, 0.71, 0.98]),
  tentativePositions: new Float32Array([
    -1.1, -0.9, 0.1,
     0.7, -0.7, 0.1,
     0.8,  0.7, 0.1,
  ]),
  tentativeConfidence: new Float32Array([0.20, 0.50, 0.68]),
  freeAirPositions: new Float32Array([
    -0.6, -0.3, 0.55,
     0.0,  0.0, 0.62,
     0.5,  0.3, 0.58,
  ]),
  unknownCandidatePositions: new Float32Array([
    -0.4, -0.2, -0.35,
     0.1,  0.1, -0.42,
     0.5,  0.2, -0.30,
  ]),
  manualControlPoints: [
    { id: 1, position: [0.55, 0.55, 0.35] as const },
  ],
  bounds: {
    min: [-1.2, -1.0, -0.5] as const,
    max: [0.9, 1.0, 0.8] as const,
  },
  voxelSizeMeters: 0.04,
  pointCount: 9,
  tentativePointCount: 3,
  knownFreeVoxelCount: 3,
  unknownCandidateVoxelCount: 3,
} as unknown as ScanPointCloudSnapshot;

function Harness() {
  const [mode, setMode] = useState<ScanMode | null>('quality');
  const [display, setDisplay] = useState<ScanDisplayOptions>(() => displayForScanMode('quality'));
  const [command, setCommand] = useState<ViewCommand>({ preset: 'perspective', sequence: 0 });

  const choose = (next: ScanMode): void => {
    setMode(next);
    setDisplay(displayForScanMode(next));
  };
  const preset = (next: ViewPreset): void => {
    setCommand((current) => ({ preset: next, sequence: current.sequence + 1 }));
  };

  return (
    <main className="app-shell">
      <section className="toolbar scan-toolbar">
        <div className="scan-mode-controls">
          <button id="mode-quality" onClick={() => choose('quality')}>Quality map</button>
          <button id="mode-surface" onClick={() => choose('surface')}>Stable surface</button>
          <button id="mode-volume" onClick={() => choose('search-volume')}>Search volume</button>
          <button id="mode-mesh" onClick={() => choose('mesh')}>Surface mesh</button>
        </div>
        <div className="scan-toolbar-group">
          <button id="view-3d" onClick={() => preset('perspective')}>3D</button>
          <button id="view-top" onClick={() => preset('top')}>Top</button>
          <button id="view-front" onClick={() => preset('front')}>Front</button>
        </div>
      </section>
      <section className="workspace" style={{ gridTemplateColumns: '1fr' }}>
        <ScanPointCloudViewport
          snapshot={snapshot}
          viewCommand={command}
          display={display}
          mode={mode}
        />
      </section>
    </main>
  );
}

createRoot(document.getElementById('root')!).render(<Harness />);
(window as unknown as { __SCAN_RESULT_FIXTURE_READY__?: boolean }).__SCAN_RESULT_FIXTURE_READY__ = true;
