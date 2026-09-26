import { describe, expect, it } from 'vitest';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

import { ScanPointCloudViewport } from '../../src/components/ScanPointCloudViewport';
import type { LocalizationSessionGeometry } from '../../src/api/deviceApi';
import type { RegistrationArtifact } from '../../src/xr/fiducial/registrationArtifact';
import type { SensorPlacementMarker, SensorPlacementNode } from '../../src/xr/sensorPlacementScene';
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

function render(
  view: ScanViewState,
  snap = snapshot(),
  preset: 'perspective' | 'top' | 'front' = 'perspective',
  sensorProps: {
    sensorMarkers?: readonly SensorPlacementMarker[];
    selectedSensorNode?: SensorPlacementNode | null;
    sensorGeometry?: LocalizationSessionGeometry | null;
    registrationArtifact?: RegistrationArtifact | null;
  } = {},
): string {
  return renderToStaticMarkup(createElement(ScanPointCloudViewport, {
    snapshot: snap,
    viewCommand: { preset, sequence: 0 },
    display: view.display,
    mode: view.mode,
    ...sensorProps,
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

  it('shows selected sensor details and the unregistered CS geometry inset caption', () => {
    const marker: SensorPlacementMarker = {
      node: 'G1', status: 'anchored', source: 'apriltag', positionScanM: [0.5, 0.2, 0.1],
      uncertaintyM: 0.012, residualM: 0.004, tagId: 1, details: 'Registered channel-sounding geometry.',
    };
    const details = render(scanViewForMode('quality'), snapshot(), 'perspective', {
      sensorMarkers: [marker], selectedSensorNode: 'G1',
    });
    expect(details).toContain('aria-label="Selected sensor marker details"');
    expect(details).toContain('G1 · anchored');
    expect(details).toContain('<dt>Source</dt><dd>apriltag</dd>');
    expect(details).toContain('<dt>Uncertainty</dt><dd>0.012 m</dd>');
    expect(details).toContain('<dt>Residual</dt><dd>0.004 m</dd>');
    expect(details).toContain('<dt>Tag ID</dt><dd>1</dd>');

    const sensorGeometry: LocalizationSessionGeometry = {
      available: true,
      pods: [
        { pod: 0, node_id: 0, computed: true, assigned: true, leveled: true, position_source: 'none', x_mm: 0, y_mm: 0, z_mm: 0 },
        { pod: 1, node_id: 1, computed: true, assigned: true, leveled: true, position_source: 'channel_sounding', x_mm: 1000, y_mm: 0, z_mm: 200 },
      ],
    };
    const invalidRegistration: RegistrationArtifact = {
      schema: 1, source: 'apriltag', family: 'tag36h11', geometry_computed_at_ms: 1, created_at_ms: 1,
      tag_size_m: { '0': 0.032, '1': 0.032, '2': 0.032, '3': 0.032, '4': 0.032 },
      bindings: [], sensor_to_scan: null, mirrored: false, residual_rms_m: 0, threshold_m: 0.01,
      per_node: [], tier: null, valid: false, reason: 'missing_main',
    };
    const inset = render(scanViewForMode('quality'), snapshot(), 'perspective', {
      sensorGeometry, registrationArtifact: invalidRegistration,
    });
    expect(inset).toContain('CS geometry — not registered to the scan');
  });
});
