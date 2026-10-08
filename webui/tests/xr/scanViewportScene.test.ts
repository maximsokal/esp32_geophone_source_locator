import { describe, expect, it } from 'vitest';

import {
  displayForScanMode,
  scanViewForMode,
  toggleScanLayer,
  type ScanDisplayOptions,
  type ScanMode,
} from '../../src/xr/scanDisplayModes';
import {
  cameraPoseForPreset,
  orbitDistanceLimits,
  prepareScanScene,
  scanViewFitExtent,
  scanDimensionsText,
  scanLayerPlan,
  scanSourceBadgeText,
  ScanSceneMaterializer,
  splitConfidenceBands,
  STABLE_CONFIDENCE,
  toViewport,
  WEAK_CONFIDENCE,
  type ScanLayerDataType,
  type ScanSceneSource,
} from '../../src/xr/scanViewportScene';
import type { SensorPlacementMarker } from '../../src/xr/sensorPlacementScene';
import type { SourceHighlightScene } from '../../src/xr/sourceHighlight';
import type { SolverCandidateVolume } from '../../src/xr/candidateVolume';
import { formatDistance, formatNumber } from '../../src/i18n/format';
import { translate, type I18nLocale } from '../../src/i18n';

/*
 * The 3D result viewport: which scanned data types (stable/tentative
 * surfels by confidence band, verified FREE air, UNKNOWN search volume,
 * surface mesh, operator pins/edges, scan bounds) become which layers, in
 * which frame, for each mode and each manual layer toggle -- and where the
 * 3D / Top / Front buttons put the camera. Pure, so no GPU is needed.
 */

function source(overrides: Partial<ScanSceneSource> = {}): ScanSceneSource {
  return {
    bounds: { min: [0, 0, 0], max: [2, 4, 1] },
    // Stable surfels: one weak, one improving, two stable.
    positions: new Float32Array([0, 0, 0, 1, 1, 0.5, 2, 4, 1, 1, 2, 0.5]),
    confidence: new Float32Array([0.2, 0.5, 0.9, 0.72]),
    // Tentative surfels: one weak.
    tentativePositions: new Float32Array([0.5, 0.5, 0.5]),
    tentativeConfidence: new Float32Array([0.1]),
    manualControlPoints: [{ id: 1, position: [1, 2, 1] } as unknown as ScanSceneSource['manualControlPoints'][number]],
    operatorEdges: [{ start: [0, 0, 0], end: [2, 0, 0] } as unknown as ScanSceneSource['operatorEdges'][number]],
    freeAirPositions: new Float32Array([1, 2, 1.5, 1.2, 2, 1.5]),
    unknownCandidatePositions: new Float32Array([1, 2, -0.5]),
    ...overrides,
  };
}

const MESH = new Float32Array([0, 0, 0, 1, 0, 0, 0, 1, 0]);
const MODES: ScanMode[] = ['quality', 'surface', 'search-volume', 'mesh'];

function dataTypes(display: ScanDisplayOptions, mesh = MESH, wireframe = false): Set<ScanLayerDataType> {
  return new Set(scanLayerPlan(prepareScanScene(source()), display, mesh, wireframe).map((layer) => layer.dataType));
}

describe('scan viewport frame', () => {
  it('maps project Z-up metres to the three.js Y-up frame, centred on the scan', () => {
    expect(toViewport(2, 5, 4, 1, 3, 1)).toEqual([1, 3, -2]);
    const prepared = prepareScanScene(source());
    expect([prepared.centerX, prepared.centerY, prepared.centerZ]).toEqual([1, 2, 0.5]);
    expect([prepared.extentX, prepared.extentY, prepared.extentZ]).toEqual([2, 4, 1]);
    expect(prepared.extent).toBe(4);
    // FREE air at project (1, 2, 1.5) sits 1 m above the centre.
    expect(Array.from(prepared.freeAirPositions.slice(0, 3))).toEqual([0, 1, -0]);
    // UNKNOWN below the surface stays below in the viewport too.
    expect(prepared.unknownPositions[1]).toBeCloseTo(-1, 6);
    expect(prepared.manualPositions).toEqual([[0, 0.5, -0]]);
    // 12 box edges x 2 endpoints x 3 coordinates.
    expect(prepared.boundsLines.length).toBe(72);
    expect(prepared.operatorEdgePositions.length).toBe(6);
  });

  it('keeps a small or unbounded scan viewable', () => {
    const tiny = prepareScanScene(source({ bounds: { min: [0, 0, 0], max: [0.1, 0.1, 0.1] } }));
    expect(tiny.extent).toBe(0.8);
    const unbounded = prepareScanScene(source({ bounds: null }));
    expect([unbounded.centerX, unbounded.centerY, unbounded.centerZ]).toEqual([0, 0, 0]);
    expect(unbounded.boundsLines.length).toBe(0);
  });

  it('splits surfels into weak / improving / stable confidence bands at the documented thresholds', () => {
    const positions = new Float32Array(15);
    const confidence = new Float32Array([WEAK_CONFIDENCE - 0.01, WEAK_CONFIDENCE + 0.01, STABLE_CONFIDENCE - 0.01, STABLE_CONFIDENCE + 0.01, 1]);
    const bands = splitConfidenceBands(positions, confidence, 0, 0, 0);
    expect([bands.weak.length, bands.improving.length, bands.stable.length]).toEqual([3, 6, 6]);
    // Confidence is stored as float32, so the exact thresholds round: a
    // stored 0.42 is 0.41999998... (still weak), a stored 0.72 is
    // 0.72000003... (already stable).
    const boundary = splitConfidenceBands(new Float32Array(6), new Float32Array([WEAK_CONFIDENCE, STABLE_CONFIDENCE]), 0, 0, 0);
    expect([boundary.weak.length, boundary.improving.length, boundary.stable.length]).toEqual([3, 0, 3]);
    // A confidence array shorter than the positions never reads past its end.
    const truncated = splitConfidenceBands(new Float32Array(9), new Float32Array([0.9]), 0, 0, 0);
    expect(truncated.stable.length).toBe(3);
  });
});

describe('scan viewport layers per data type and mode', () => {
  it('QUALITY MAP shows both surfel populations, the mesh overlay, pins and bounds', () => {
    expect(dataTypes(displayForScanMode('quality'))).toEqual(new Set<ScanLayerDataType>([
      'surface-mesh', 'tentative-surface', 'stable-surface', 'operator-edges', 'operator-pins', 'scan-bounds',
    ]));
  });

  it('SEARCH VOLUME shows the empty (FREE) and unknown space around the stable surface, no mesh', () => {
    expect(dataTypes(displayForScanMode('search-volume'))).toEqual(new Set<ScanLayerDataType>([
      'unknown-space', 'free-air', 'stable-surface', 'operator-edges', 'operator-pins', 'scan-bounds',
    ]));
  });

  it('SURFACE MESH draws the mesh as a wireframe overlay and no points', () => {
    const plan = scanLayerPlan(prepareScanScene(source()), displayForScanMode('mesh'), MESH, true);
    expect(plan.some((layer) => layer.kind === 'points')).toBe(false);
    const mesh = plan.find((layer) => layer.kind === 'mesh');
    expect(mesh).toMatchObject({ dataType: 'surface-mesh', wireframe: true });
  });

  it('gives each mode a distinct set of visible data types', () => {
    const signatures = MODES.map((mode) => [...dataTypes(displayForScanMode(mode))].sort().join(','));
    expect(new Set(signatures).size).toBe(MODES.length);
  });

  it('draws the volume layers first and without depth test, surfels by confidence colour', () => {
    const plan = scanLayerPlan(prepareScanScene(source()), displayForScanMode('search-volume'), MESH, false);
    const ids = plan.map((layer) => layer.id);
    expect(ids.indexOf('unknown-space')).toBeLessThan(ids.indexOf('free-air'));
    expect(ids.indexOf('free-air')).toBeLessThan(ids.indexOf('stable-surface-stable'));
    const unknown = plan.find((layer) => layer.id === 'unknown-space');
    const free = plan.find((layer) => layer.id === 'free-air');
    expect(unknown).toMatchObject({ kind: 'points', depthTest: false, renderOrder: 1, color: '#f59e0b' });
    expect(free).toMatchObject({ kind: 'points', depthTest: false, renderOrder: 2, color: '#38bdf8' });
    const colors = new Set(plan.filter((layer) => layer.kind === 'points').map((layer) => (layer.kind === 'points' ? layer.color : '')));
    expect(colors.has('#ff3d28') && colors.has('#facc15') && colors.has('#2ee6a6')).toBe(true);
    // Tentative surfels are drawn fainter than stable ones.
    const quality = scanLayerPlan(prepareScanScene(source()), displayForScanMode('quality'), MESH, false);
    const tentativeWeak = quality.find((layer) => layer.id === 'tentative-surface-weak');
    const stableWeak = quality.find((layer) => layer.id === 'stable-surface-weak');
    expect(tentativeWeak && tentativeWeak.kind === 'points' ? tentativeWeak.opacity : 1)
      .toBeLessThan(stableWeak && stableWeak.kind === 'points' ? stableWeak.opacity : 0);
  });

  it('omits data types that are empty in this scan', () => {
    const empty = source({ freeAirPositions: new Float32Array(0), unknownCandidatePositions: new Float32Array(0), manualControlPoints: [], operatorEdges: [] });
    const plan = scanLayerPlan(prepareScanScene(empty), displayForScanMode('search-volume'), new Float32Array(0), false);
    expect(plan.map((layer) => layer.dataType)).not.toContain('free-air');
    expect(plan.map((layer) => layer.dataType)).not.toContain('unknown-space');
    expect(plan.map((layer) => layer.dataType)).not.toContain('operator-pins');
    const noMesh = scanLayerPlan(prepareScanScene(source()), displayForScanMode('mesh'), new Float32Array(0), true);
    expect(noMesh.map((layer) => layer.dataType)).not.toContain('surface-mesh');
  });

  it('adds the sensor marker layer only when markers have scan-frame positions', () => {
    const prepared = prepareScanScene(source());
    const marker: SensorPlacementMarker = {
      node: 'G1', status: 'anchored', source: 'apriltag', positionScanM: [1, 2, 3],
      uncertaintyM: 0.04, residualM: 0.01, tagId: 1, details: 'registered',
    };
    const withMarker = scanLayerPlan(prepared, displayForScanMode('quality'), MESH, false, [marker]);
    const layer = withMarker.find((item) => item.kind === 'sensor-markers');
    expect(layer).toMatchObject({ dataType: 'sensor-markers', renderOrder: 60 });
    expect(layer?.kind === 'sensor-markers' ? layer.markers[0].positionViewport : null)
      .toEqual(toViewport(1, 2, 3, prepared.centerX, prepared.centerY, prepared.centerZ));

    const unregistered = { ...marker, status: 'cs_unregistered' as const, positionScanM: null };
    const withoutPosition = scanLayerPlan(prepared, displayForScanMode('quality'), MESH, false, [unregistered]);
    expect(withoutPosition.map((item) => item.dataType)).not.toContain('sensor-markers');
    expect(scanLayerPlan(prepared, displayForScanMode('quality'), MESH, false).map((item) => item.dataType))
      .not.toContain('sensor-markers');
  });
});

describe('scan viewport layer toggles', () => {
  const toggles: Array<keyof ScanDisplayOptions> = ['goodPoints', 'badPoints', 'mesh', 'freeAir', 'unknownSpace', 'manualPoints', 'dimensions'];

  it.each(MODES)('each toggle in %s flips exactly its own layers and becomes CUSTOM LAYERS', (mode) => {
    const prepared = prepareScanScene(source());
    const start = scanViewForMode(mode);
    expect(start.mode).toBe(mode);
    for (const toggle of toggles) {
      const next = toggleScanLayer(start, toggle);
      expect(next.mode).toBeNull();
      for (const other of toggles) {
        if (other !== toggle) expect(next.display[other]).toBe(start.display[other]);
      }
      expect(next.display[toggle]).toBe(!start.display[toggle]);
      const before = scanLayerPlan(prepared, start.display, MESH, false);
      const after = scanLayerPlan(prepared, next.display, MESH, false);
      const untouched = (plan: typeof before) => plan.filter((layer) => layer.toggle !== toggle).map((layer) => layer.id);
      expect(untouched(after)).toEqual(untouched(before));
      const own = (plan: typeof before) => plan.filter((layer) => layer.toggle === toggle).length;
      expect(own(after) > 0).toBe(next.display[toggle]);
      // Toggling twice restores the layers (but not the preset label).
      const back = toggleScanLayer(next, toggle);
      expect(back.display).toEqual(start.display);
      expect(back.mode).toBeNull();
    }
  });

  it('never lets a toggle mutate the preset it started from', () => {
    const start = scanViewForMode('quality');
    toggleScanLayer(start, 'mesh');
    expect(start.display.mesh).toBe(true);
    expect(displayForScanMode('quality').mesh).toBe(true);
  });
});

describe('3D / Top / Front camera switching', () => {
  it('puts the camera on the documented axes, scaled to the scan', () => {
    const extent = 4;
    const distance = extent * 1.75;
    const perspective = cameraPoseForPreset('perspective', extent);
    const top = cameraPoseForPreset('top', extent);
    const front = cameraPoseForPreset('front', extent);
    expect(perspective.position).toEqual([distance * 0.66, distance * 0.52, distance * 0.72]);
    expect(perspective.up).toEqual([0, 1, 0]);
    // Top: straight down the viewport Y (project Z) axis, project +Y up on screen.
    expect(top.position).toEqual([0, distance, 0.001]);
    expect(top.up).toEqual([0, 0, -1]);
    // Front: along viewport +Z, level with the scan.
    expect(front.position).toEqual([0, extent * 0.08, distance]);
    expect(front.up).toEqual([0, 1, 0]);
    for (const pose of [perspective, top, front]) expect(pose.target).toEqual([0, 0, 0]);
  });

  it('switches presets deterministically and back to the same 3D pose', () => {
    const first = cameraPoseForPreset('perspective', 2.5);
    cameraPoseForPreset('top', 2.5);
    cameraPoseForPreset('front', 2.5);
    expect(cameraPoseForPreset('perspective', 2.5)).toEqual(first);
  });

  it('keeps the camera outside a tiny scan and within orbit limits', () => {
    const small = cameraPoseForPreset('top', 0.2);
    expect(small.position[1]).toBe(1.5);
    const limits = orbitDistanceLimits(4);
    expect(limits).toEqual({ minDistance: 0.4, maxDistance: 40 });
    expect(orbitDistanceLimits(0.5)).toEqual({ minDistance: 0.18, maxDistance: 18 });
    const perspective = cameraPoseForPreset('perspective', 4);
    const radius = Math.hypot(...perspective.position);
    expect(radius).toBeGreaterThan(limits.minDistance);
    expect(radius).toBeLessThan(limits.maxDistance);
  });

  it('fits active source and uncertainty, adds candidate depth only for the search-volume view', () => {
    const thinSurface = prepareScanScene(source({ bounds: { min: [0, 0, 0], max: [0.1, 0.1, 0] } }));
    const sourceLayer = {
      reason: null,
      primary: {
        run: {} as never,
        positionProjectM: [0.05, 0.05, -1.5],
        primary: true,
        voxel: { index: 0, coordinates: [0, 0, 0], center: [0.05, 0.05, -1.5] },
        edgeMarker: null,
      },
      uncertaintyVoxels: [{ index: 1, coordinates: [0, 0, 0], center: [0.05, 0.05, -2] }],
      secondary: [],
      voxelSizeMeters: 0.1,
      historyRuns: [],
      staleRuns: [],
    } as unknown as SourceHighlightScene;
    const candidateVolume: SolverCandidateVolume = {
      min: [0, 0, -2.5], max: [0.1, 0.1, 0], voxelSize: 0.1, candidateDepthUnavailable: false,
    };

    const surfaceOnlyExtent = scanViewFitExtent(thinSurface, null, candidateVolume, false);
    const sourceExtent = scanViewFitExtent(thinSurface, sourceLayer, candidateVolume, false);
    const searchVolumeExtent = scanViewFitExtent(thinSurface, sourceLayer, candidateVolume, true);
    expect(surfaceOnlyExtent).toBe(thinSurface.extent);
    expect(sourceExtent).toBeGreaterThanOrEqual(4);
    expect(searchVolumeExtent).toBeGreaterThanOrEqual(5);
    expect(orbitDistanceLimits(searchVolumeExtent).maxDistance)
      .toBeGreaterThan(orbitDistanceLimits(surfaceOnlyExtent).maxDistance);
  });

  it('matches the former boxed splitter byte-for-byte across truncated and threshold inputs', () => {
    const positions = new Float32Array([
      -1.2, 0.4, 2.5, 0.2, -0.8, 1.3, 4.1, 2.2, -0.5, 0.9, 1.7, 3.3, 2.1, 0, 0.4,
    ]);
    const confidence = new Float32Array([
      WEAK_CONFIDENCE - 0.01,
      WEAK_CONFIDENCE,
      STABLE_CONFIDENCE - 0.01,
      STABLE_CONFIDENCE,
    ]);
    const weak: number[] = [];
    const improving: number[] = [];
    const stable: number[] = [];
    for (let index = 0; index < Math.min(Math.floor(positions.length / 3), confidence.length); index += 1) {
      const offset = index * 3;
      const transformed = toViewport(positions[offset], positions[offset + 1], positions[offset + 2], 0.4, -0.2, 1.1);
      const target = confidence[index] < WEAK_CONFIDENCE ? weak
        : confidence[index] < STABLE_CONFIDENCE ? improving
          : stable;
      target.push(transformed[0], transformed[1], transformed[2]);
    }
    const actual = splitConfidenceBands(positions, confidence, 0.4, -0.2, 1.1);
    expect(actual.weak).toEqual(Float32Array.from(weak));
    expect(actual.improving).toEqual(Float32Array.from(improving));
    expect(actual.stable).toEqual(Float32Array.from(stable));
  });

  it('materializes hidden volume layers on demand, reuses other layers, and drops caches on generation change', () => {
    const snapshot = source({
      freeAirPositions: new Float32Array(12).fill(2),
      unknownCandidatePositions: new Float32Array(36).fill(3),
    });
    const materializer = new ScanSceneMaterializer();
    const qualityNeeds = {
      good: true, bad: true, freeAir: false, unknownSpace: false, manualPoints: true, dimensions: true,
    };
    const quality = materializer.prepare(snapshot, 'scan-a', qualityNeeds);
    expect(quality.freeAirPositions).toHaveLength(0);
    expect(quality.unknownPositions).toHaveLength(0);

    const search = materializer.prepare(snapshot, 'scan-a', { ...qualityNeeds, freeAir: true, unknownSpace: true });
    expect(search.freeAirPositions).toHaveLength(snapshot.freeAirPositions.length);
    expect(search.unknownPositions).toHaveLength(snapshot.unknownCandidatePositions.length);
    expect(search.good).toBe(quality.good);
    const unrelatedToggle = materializer.prepare(snapshot, 'scan-a', {
      ...qualityNeeds, freeAir: true, unknownSpace: true, dimensions: false,
    });
    expect(unrelatedToggle.freeAirPositions).toBe(search.freeAirPositions);
    expect(unrelatedToggle.unknownPositions).toBe(search.unknownPositions);
    expect(unrelatedToggle.good).toBe(search.good);

    const nextGeneration = materializer.prepare(snapshot, 'scan-b', { ...qualityNeeds, freeAir: true, unknownSpace: true });
    expect(nextGeneration.freeAirPositions).not.toBe(search.freeAirPositions);
    expect(nextGeneration.unknownPositions).not.toBe(search.unknownPositions);
    materializer.release();
  });
});

describe('scan viewport overlay text', () => {
  it.each(['en', 'uk'] as const)('formats every scanned data type count and dimension in %s', (locale: I18nLocale) => {
    const snapshot = {
      pointCount: 1234, knownFreeVoxelCount: 56, unknownCandidateVoxelCount: 7,
      manualControlPoints: [{} as never, {} as never], operatorEdges: [{} as never],
    };
    expect(scanSourceBadgeText(snapshot, locale)).toBe(`${translate(locale, 'scan.badge.surface')} ${formatNumber(locale, 1234)} · ${translate(locale, 'scan.badge.free')} ${formatNumber(locale, 56)} · ${translate(locale, 'scan.badge.unknown')} ${formatNumber(locale, 7)} · ${translate(locale, 'scan.badge.pins')} ${formatNumber(locale, 2)} · ${translate(locale, 'scan.badge.edges')} ${formatNumber(locale, 1)}`);
    const dimensions = prepareScanScene(source());
    expect(scanDimensionsText(dimensions, locale)).toBe(`X ${formatDistance(locale, dimensions.extentX)} · Y ${formatDistance(locale, dimensions.extentY)} · Z ${formatDistance(locale, dimensions.extentZ)}`);
  });
});
