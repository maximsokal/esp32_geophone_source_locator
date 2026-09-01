import { useMemo, useRef, useState } from 'react';
import { createRoot } from 'react-dom/client';

import '../src/scan.css';
import {
  ManualControlPointStore,
  type ProjectPoint,
} from '../src/xr/manualControlPoints';
import {
  previewManualPlacementFromStableSurface,
  manualPreviewLabel,
  type ManualPlacementPreview,
} from '../src/xr/manualPlacementPreview';
import {
  registerManualPlacementResolver,
  solveManualPlacementOnStableSurface,
  type ManualPlacementSurfaceSnapshot,
} from '../src/xr/manualPlacementSurface';

interface MutableCloud {
  positions: number[];
  normals: number[];
  confidence: number[];
}

function mutableCloud(): MutableCloud {
  return { positions: [], normals: [], confidence: [] };
}

function add(
  cloud: MutableCloud,
  position: ProjectPoint,
  normal: ProjectPoint,
  confidence = 0.94,
): void {
  cloud.positions.push(...position);
  cloud.normals.push(...normal);
  cloud.confidence.push(confidence);
}

function addEdge(cloud: MutableCloud, centerX: number): void {
  for (const y of [-0.08, -0.04, 0, 0.04, 0.08]) {
    for (const z of [-0.04, 0, 0.04]) add(cloud, [centerX, y, z], [1, 0, 0]);
  }
  for (const x of [-0.08, -0.04, 0, 0.04, 0.08]) {
    for (const z of [-0.04, 0, 0.04]) add(cloud, [centerX + x, 0, z], [0, 1, 0]);
  }
}

function addCorner(cloud: MutableCloud, centerX: number): void {
  addEdge(cloud, centerX);
  for (const x of [-0.08, -0.04, 0, 0.04, 0.08]) {
    for (const y of [-0.04, 0, 0.04]) add(cloud, [centerX + x, y, 0], [0, 0, 1]);
  }
}

function createSnapshot(): ManualPlacementSurfaceSnapshot {
  const cloud = mutableCloud();
  addEdge(cloud, -0.45);
  addCorner(cloud, 0.45);
  return {
    positions: Float32Array.from(cloud.positions),
    normals: Float32Array.from(cloud.normals),
    confidence: Float32Array.from(cloud.confidence),
    pointCount: cloud.confidence.length,
    voxelSizeMeters: 0.04,
  };
}

const snapshot = createSnapshot();
registerManualPlacementResolver((candidate) => solveManualPlacementOnStableSurface(candidate, snapshot));

function project(point: ProjectPoint) {
  return {
    u: 0.5 + point[0] * 0.56,
    v: 0.5 - point[1] * 0.5,
    visible: true,
  };
}

function normalized(event: React.PointerEvent<HTMLDivElement>): readonly [number, number] {
  const rect = event.currentTarget.getBoundingClientRect();
  return [
    Math.max(0, Math.min(1, (event.clientX - rect.left) / Math.max(1, rect.width))),
    Math.max(0, Math.min(1, (event.clientY - rect.top) / Math.max(1, rect.height))),
  ];
}

function distanceLabel(preview: ManualPlacementPreview | null): string {
  const point = preview?.resolution.position;
  if (point == null) return '';
  const meters = Math.hypot(point[0], point[1], 1 - point[2]);
  return `${meters.toFixed(2)} m`;
}

function Fixture() {
  const store = useMemo(() => new ManualControlPointStore(), []);
  const holding = useRef(false);
  const [preview, setPreview] = useState<ManualPlacementPreview | null>(null);
  const [pinCount, setPinCount] = useState(0);
  const [lastMode, setLastMode] = useState('none');

  const updatePreview = (event: React.PointerEvent<HTMLDivElement>): ManualPlacementPreview => {
    const [u, v] = normalized(event);
    const next = previewManualPlacementFromStableSurface(u, v, snapshot, project);
    setPreview(next);
    return next;
  };

  const label = manualPreviewLabel(preview);
  const state = label === 'CORNER LOCK' ? 'corner' : label === 'EDGE LOCK' ? 'edge' : 'scan-more';
  const distance = distanceLabel(preview);

  return (
    <main
      style={{
        minHeight: '100vh',
        margin: 0,
        padding: 12,
        boxSizing: 'border-box',
        color: '#e2e8f0',
        background: '#020617',
        fontFamily: 'system-ui, sans-serif',
      }}
    >
      <div style={{ marginBottom: 10, fontSize: 13, fontWeight: 800 }}>MANUAL PLACEMENT MOBILE E2E</div>
      <div
        id="manual-placement-surface"
        data-placement-state={state}
        data-placement-label={label}
        data-pin-count={pinCount}
        data-last-mode={lastMode}
        style={{
          position: 'relative',
          width: '100%',
          height: '72vh',
          minHeight: 520,
          overflow: 'hidden',
          touchAction: 'none',
          userSelect: 'none',
          border: '1px solid #334155',
          borderRadius: 16,
          background: 'radial-gradient(circle at 50% 50%, #172033, #07101f 72%)',
        }}
        onPointerDown={(event) => {
          event.preventDefault();
          holding.current = true;
          try { event.currentTarget.setPointerCapture(event.pointerId); } catch { /* best effort */ }
          updatePreview(event);
        }}
        onPointerMove={(event) => {
          if (!holding.current) return;
          updatePreview(event);
        }}
        onPointerUp={(event) => {
          if (!holding.current) return;
          const next = updatePreview(event);
          holding.current = false;
          try { event.currentTarget.releasePointerCapture(event.pointerId); } catch { /* best effort */ }
          if (!next.resolution.accepted || next.seedPosition == null) return;
          const stored = store.addOrSnap(next.seedPosition);
          setPinCount(store.size);
          setLastMode(stored.placementMode ?? 'none');
        }}
        onPointerCancel={() => {
          holding.current = false;
        }}
      >
        <div style={{ position: 'absolute', left: '25%', top: '14%', transform: 'translateX(-50%)', color: '#7dd3fc', fontSize: 12 }}>EDGE AREA</div>
        <div style={{ position: 'absolute', left: '50%', top: '14%', transform: 'translateX(-50%)', color: '#fde68a', fontSize: 12 }}>NO DATA</div>
        <div style={{ position: 'absolute', left: '75%', top: '14%', transform: 'translateX(-50%)', color: '#86efac', fontSize: 12 }}>CORNER AREA</div>

        <div
          className={`xr-manual-reticle ${state === 'corner' ? 'lock-corner' : state === 'edge' ? 'lock-edge' : 'scan-more'}`}
          style={{
            left: `${(preview?.u ?? 0.5) * 100}%`,
            top: `${(preview?.v ?? 0.5) * 100}%`,
          }}
          aria-hidden="true"
        />
        <div
          className="xr-manual-cursor-state"
          style={{
            left: `${(preview?.u ?? 0.5) * 100}%`,
            top: `${(preview?.v ?? 0.5) * 100}%`,
          }}
        >
          {label}{distance ? ` · ${distance}` : ''}
        </div>
      </div>
      <div id="fixture-summary" style={{ marginTop: 10, fontFamily: 'monospace', fontSize: 12 }}>
        {label} · pins {pinCount} · last {lastMode}
      </div>
    </main>
  );
}

createRoot(document.getElementById('root')!).render(<Fixture />);
(window as Window & { __MANUAL_PLACEMENT_UI_READY__?: boolean }).__MANUAL_PLACEMENT_UI_READY__ = true;
