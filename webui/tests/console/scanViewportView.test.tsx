import { describe, expect, it } from 'vitest';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

import { ScanPointCloudViewport } from '../../src/components/ScanPointCloudViewport';
import {
  displayFingerprint,
  scanModeLabel,
  scanViewForMode,
  toggleScanLayer,
  type ScanMode,
  type ScanViewState,
} from '../../src/xr/scanDisplayModes';
import type { ScanPointCloudSnapshot } from '../../src/xr/webxrRuntime';

/*
 * The measured 3D search-volume viewport as the operator sees it, rendered
 * on the server: the WebGL layers themselves need a GPU (their mapping is
 * covered by tests/xr/scanViewportScene.test.ts), but the shell, the
 * mode/layer state it exposes and every overlay are plain DOM.
 */

function snapshot(overrides: Partial<ScanPointCloudSnapshot> = {}): ScanPointCloudSnapshot {
  return {
    bounds: { min: [0, 0, 0], max: [2.5, 1.25, 0.8] },
    positions: new Float32Array([0, 0, 0, 1, 1, 0.5]),
    confidence: new Float32Array([0.3, 0.9]),
    tentativePositions: new Float32Array(0),
    tentativeConfidence: new Float32Array(0),
    manualControlPoints: [],
    operatorEdges: [],
    freeAirPositions: new Float32Array([1, 1, 1]),
    unknownCandidatePositions: new Float32Array([1, 1, -1]),
    pointCount: 2,
    knownFreeVoxelCount: 1,
    unknownCandidateVoxelCount: 1,
    tsdfPointCount: 0,
    tsdfPositions: new Float32Array(0),
    voxelSizeMeters: 0.04,
    occupancyVoxelSizeMeters: 0.18,
    ...overrides,
  } as ScanPointCloudSnapshot;
}

function render(view: ScanViewState, snap = snapshot(), preset: 'perspective' | 'top' | 'front' = 'perspective'): string {
  return renderToStaticMarkup(createElement(ScanPointCloudViewport, {
    snapshot: snap,
    viewCommand: { preset, sequence: 0 },
    display: view.display,
    mode: view.mode,
  }));
}

const MODES: ScanMode[] = ['quality', 'surface', 'search-volume', 'mesh'];

describe('ScanPointCloudViewport', () => {
  it.each(MODES)('exposes the %s preset and its layer fingerprint', (mode) => {
    const view = scanViewForMode(mode);
    const html = render(view);
    expect(html).toContain(`data-scan-mode="${mode}"`);
    expect(html).toContain(`data-layer-fingerprint="${displayFingerprint(view.display)}"`);
    expect(html).toContain(`${scanModeLabel(mode)} · ${displayFingerprint(view.display).toUpperCase()}`);
  });

  it('reports every scanned data type count in the source badge', () => {
    const html = render(scanViewForMode('search-volume'), snapshot({
      pointCount: 4321, knownFreeVoxelCount: 900, unknownCandidateVoxelCount: 77,
    }));
    expect(html).toContain('SURFACE 4,321 · FREE 900 · UNKNOWN 77 · PINS 0 · EDGES 0');
  });

  it('shows dimensions only while the dimensions layer is on', () => {
    const view = scanViewForMode('quality');
    expect(render(view)).toContain('X 2.50 m · Y 1.25 m · Z 0.80 m');
    const hidden = toggleScanLayer(view, 'dimensions');
    const html = render(hidden);
    expect(html).not.toContain('scan-dimension-overlay');
    // A hand toggle is labelled as custom, never as the preset it came from.
    expect(html).toContain('data-scan-mode="custom"');
    expect(html).toContain('CUSTOM LAYERS');
  });

  it('shows the mesh status only while the mesh layer is on', () => {
    const tooFew = render(scanViewForMode('mesh'));
    expect(tooFew).toContain('MESH · SCAN MORE FOR CERTIFIED GEOMETRY');
    expect(tooFew).toContain('data-mesh-quality="pending"');
    expect(render(scanViewForMode('search-volume'))).not.toContain('scan-mesh-overlay');
    // Enough certified surfels: the mesh is being built.
    const building = render(scanViewForMode('mesh'), snapshot({ tsdfPointCount: 100000 }));
    expect(building).toContain('MESH · BUILDING…');
  });

  it('keeps the operator-facing 3D view shell for every camera preset', () => {
    for (const preset of ['perspective', 'top', 'front'] as const) {
      const html = render(scanViewForMode('quality'), snapshot(), preset);
      expect(html).toContain('aria-label="Measured WebXR rescue search volume"');
      expect(html).toContain('1 finger = orbit · 2 fingers = zoom/pan · X/Y horizontal · Z height');
    }
  });
});
