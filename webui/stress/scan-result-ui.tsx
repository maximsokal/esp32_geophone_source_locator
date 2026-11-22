import { useEffect, useMemo, useState } from 'react';
import { createRoot } from 'react-dom/client';

import '../src/styles.css';
import '../src/scan.css';
import '../src/console.css';
import { ScanPointCloudViewport, type Esp32VoxelView } from '../src/components/ScanPointCloudViewport';
import { ScanRepresentationSelect, type ScanRepresentation } from '../src/components/ScanRepresentationSelect';
import { buildVoxelPreviewArtifact, decodeVoxelPreviewArtifact } from '../src/xr/voxelPreview';
import { SceneViewport, type ViewCommand, type ViewPreset } from '../src/components/SceneViewport';
import { XrRuntimePanel } from '../src/components/XrRuntimePanel';
import LocalizationOverlayScene from '../src/features/objects/LocalizationOverlayScene';
import type { LocalizationOverlayModel } from '../src/domain/localizationOverlay';
import { demoScene } from '../src/data/demoScene';
import { LocaleProvider } from '../src/i18n';
import { encodeScanArtifact } from '../src/xr/scanArtifact';
import { ScanDecodeClient } from '../src/xr/scanDecodeClient';
import { makeValidScanSnapshot } from '../tests/xr/support/validScanSnapshot';
import record from '../tests/e2e/fixtures/graphics-scan.json';
import { FakeXrLayer, FakeXrSession, FakeXrSystem } from '../tests/xr/support/xrFakes';

// Only the unavailable immersive-session protocol is simulated; GL stays native.
if (new URLSearchParams(location.search).has('syntheticXR')) {
  const xr = new FakeXrSystem();
  const sessions: FakeXrSession[] = [];
  const requestSession = xr.requestSession.bind(xr);
  xr.requestSession = () => { const session = new FakeXrSession(); sessions.push(session); xr.queue(session); return requestSession(); };
  Object.defineProperty(navigator, 'xr', { configurable: true, value: xr });
  Object.assign(window, { XRWebGLLayer: FakeXrLayer, XRWebGLBinding: undefined,
    __syntheticXR: { end: () => sessions.at(-1)?.interrupt(),
      state: () => ({ liveSessions: xr.liveSessions,
        pendingFrames: sessions.reduce((sum, session) => sum + session.liveFrameCallbacks, 0),
        listeners: sessions.reduce((sum, session) => sum + [...session.listeners.values()].reduce((n, set) => n + set.size, 0), 0) }) } });
  for (const prototype of [WebGLRenderingContext.prototype, WebGL2RenderingContext.prototype]) {
    Object.defineProperty(prototype, 'makeXRCompatible', { configurable: true, value: async () => {} });
  }
}
import {
  displayForScanMode,
  type ScanMode,
} from '../src/xr/scanDisplayModes';
import type { ScanPointCloudSnapshot } from '../src/xr/webxrRuntime';

const positions = Float32Array.from(record.positions);
const normals = Float32Array.from(record.normals);
const confidence = new Float32Array(positions.length / 3).fill(0.92);
const recordedSnapshot: ScanPointCloudSnapshot = {
  ...makeValidScanSnapshot(), positions, normals, confidence,
  normalConfidence: confidence.slice(), pointCount: confidence.length, rawPointCount: confidence.length,
  bounds: { min: [-0.32, -0.32, -0.05], max: [0.32, 0.32, 0.15] },
  fullBounds: { min: [-0.32, -0.32, -0.05], max: [0.32, 0.32, 0.15] },
  tsdfPositions: positions.slice(), tsdfNormals: normals.slice(), tsdfConfidence: confidence.slice(),
  tsdfResidualMeters: new Float32Array(confidence.length).fill(0.005),
  tsdfNormalConfidence: confidence.slice(), tsdfCoverageConfidence: confidence.slice(),
  tsdfCoverageReady: new Uint8Array(confidence.length).fill(1), tsdfPointCount: confidence.length,
  occupiedSurfacePositions: positions.slice(), occupiedSurfaceVoxelCount: confidence.length,
  manualControlPoints: [{ id: 1, position: [0.2, 0.2, 0.1] }],
};

function Harness() {
  const [mode, setMode] = useState<ScanMode>('quality');
  const [representation, setRepresentation] = useState<ScanRepresentation>('mesh');
  const [command, setCommand] = useState<ViewCommand>({ preset: 'perspective', sequence: 0 });
  const [snapshot, setSnapshot] = useState<ScanPointCloudSnapshot | null>(null);
  const [renderer, setRenderer] = useState('scan');
  const [mounted, setMounted] = useState(true);
  const [expanded, setExpanded] = useState(false);
  const [tableOnly, setTableOnly] = useState(false);
  const [active, setActive] = useState(false);
  const [error, setError] = useState('');

  useEffect(() => {
    const client = new ScanDecodeClient();
    void client.decode(encodeScanArtifact(recordedSnapshot), {
      transferInput: true, onFallback: () => setError('Unexpected decode fallback'),
    }).then(result => setSnapshot(result.snapshot)).catch(reason => {
      if (!(reason instanceof Error && reason.name === 'ScanDecodeCancelledError')) setError(String(reason));
    });
    return () => client.dispose();
  }, []);
  /* The voxels come from the production builder and decoder, exactly the path the bytes take to the ESP32 and back into the viewer. */
  const voxelView = useMemo<Esp32VoxelView | null>(() => {
    if (representation !== 'esp32-preview' || snapshot === null) return null;
    const bytes = buildVoxelPreviewArtifact(snapshot, encodeScanArtifact(recordedSnapshot));
    return { artifact: decodeVoxelPreviewArtifact(bytes), status: 'prepared' };
  }, [representation, snapshot]);
  const preset = (next: ViewPreset): void => {
    setCommand((current) => ({ preset: next, sequence: current.sequence + 1 }));
  };
  const overlay: LocalizationOverlayModel = {
    sensors: [{ pod: 0, label: 'MAIN', position: { x_m: 0, y_m: 0, z_m: 0 } },
      { pod: 1, label: 'G1', position: { x_m: expanded ? 5 : 1, y_m: 1, z_m: 0 } }],
    sectors: [], unavailable: [], selectedMethod: null,
  };
  const visibleSnapshot = snapshot === null || !expanded ? snapshot : {
    ...snapshot, bounds: { min: [-1, -1, -1] as const, max: [1, 1, 1] as const },
  };

  return (
    <main className="app-shell">
      <p>Recorded synthetic scan · actual production renderers · no camera/device input</p>
      <section className="toolbar scan-toolbar">
        <div className="scan-mode-controls">
          {(['quality', 'surface', 'search-volume', 'mesh'] as const).map(next =>
            <button key={next} id={`mode-${next === 'search-volume' ? 'volume' : next}`} onClick={() => { setRepresentation('mesh'); setMode(next); }}>{next}</button>)}
          {/* The production select (the harness keeps its own Mesh button for the older tests; the App has only the select). */}
          <ScanRepresentationSelect representation={representation} meshActive={mode === 'mesh'} preview={snapshot === null ? 'absent' : 'ready'}
            onChoose={(choice) => { setRepresentation(choice); if (choice === 'mesh') setMode('mesh'); }} />
        </div>
        <div className="scan-mode-controls">
          <button id="view-3d" onClick={() => preset('perspective')}>3D</button>
          <button id="view-top" onClick={() => preset('top')}>Top</button>
          <button id="view-front" onClick={() => preset('front')}>Front</button>
          <button id="mount" onClick={() => setMounted(value => !value)}>{mounted ? 'Close' : 'Open'}</button>
          <button id="extent" onClick={() => setExpanded(value => !value)}>Change extent</button>
          <button id="table" onClick={() => setTableOnly(value => !value)}>2D / 3D</button>
          <select id="renderer" aria-label="Production renderer" value={renderer} onChange={event => setRenderer(event.target.value)}>
            {['scan', 'sensors', 'localization', 'runtime'].map(value => <option key={value}>{value}</option>)}
          </select>
        </div>
      </section>
      <output id="voxel-state" data-cells={voxelView?.artifact.voxelCount ?? 0} data-faces-rendered={voxelView === null ? 0 : 1}>{voxelView === null ? 'mesh' : 'esp32-preview'}</output>
      <output id="fixture-state" data-loaded={snapshot !== null} data-active={active}>{error || (snapshot ? `${snapshot.pointCount} recorded points` : 'Loading emitted decode Worker')}</output>
      {mounted && snapshot && renderer === 'runtime' && !tableOnly && <XrRuntimePanel onSessionActiveChange={setActive} />}
      <section className="workspace" style={{ gridTemplateColumns: '1fr' }}>
        {mounted && visibleSnapshot && renderer !== 'runtime' && (tableOnly
          ? <table><caption>Recorded scan summary (2D)</caption><tbody><tr><th>Points</th><td>{visibleSnapshot.pointCount}</td></tr></tbody></table>
          : renderer === 'scan'
            ? <ScanPointCloudViewport snapshot={visibleSnapshot} viewCommand={command} display={displayForScanMode(mode)} mode={mode} esp32Voxels={voxelView} />
            : renderer === 'sensors'
              ? <SceneViewport scene={demoScene} selectedId="MAIN" viewCommand={command} onSelect={() => {}} />
              : renderer === 'localization'
                ? <LocalizationOverlayScene model={overlay} />
                : null)}
      </section>
    </main>
  );
}

createRoot(document.getElementById('root')!).render(<LocaleProvider><Harness /></LocaleProvider>);
