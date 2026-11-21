import type { ScanDisplayOptions } from './scanDisplayModes';
import type { ScanPointCloudSnapshot } from './webxrRuntime';
import type { SensorPlacementMarker } from './sensorPlacementScene';
import type { SourceHighlightScene } from './sourceHighlight';
import type { SolverCandidateVolume } from './candidateVolume';
import type { I18nLocale } from '../i18n';
import type { ScanSceneMaterializationNeeds } from './postScanAdmission';
import { formatDistance, formatNumber } from '../i18n/format';
import { translate } from '../i18n';
import { uiTheme } from '../theme';

/**
 * Pure scene preparation for ScanPointCloudViewport: which scanned data
 * types become which viewport layers, in which frame, colour and draw order,
 * and where the camera goes for each view preset. Kept free of React/WebGL
 * so the mapping from a saved scan to what the operator sees is testable
 * without a GPU.
 */

export type ViewportPreset = 'perspective' | 'top' | 'front';

export type Vec3Tuple = readonly [number, number, number];

export interface QualityBands {
  weak: Float32Array;
  improving: Float32Array;
  stable: Float32Array;
}

export interface PreparedScanScene {
  good: QualityBands;
  bad: QualityBands;
  manualPositions: ReadonlyArray<Vec3Tuple>;
  operatorEdgePositions: Float32Array;
  freeAirPositions: Float32Array;
  unknownPositions: Float32Array;
  boundsLines: Float32Array;
  centerX: number;
  centerY: number;
  centerZ: number;
  extent: number;
  extentX: number;
  extentY: number;
  extentZ: number;
}

const EMPTY_POSITIONS = new Float32Array(0);
const EMPTY_BANDS: QualityBands = { weak: EMPTY_POSITIONS, improving: EMPTY_POSITIONS, stable: EMPTY_POSITIONS };
const ALL_MATERIALIZATION_NEEDS: ScanSceneMaterializationNeeds = {
  good: true, bad: true, freeAir: true, unknownSpace: true, manualPoints: true, dimensions: true,
};

/** Confidence below this is WEAK, below STABLE_CONFIDENCE is IMPROVING. */
export const WEAK_CONFIDENCE = 0.42;
export const STABLE_CONFIDENCE = 0.72;

export function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}

/** WebXR local-floor (Y up) to the scan's project frame (Z up). */
export function webXrLocalFloorToProject(x: number, y: number, z: number): Vec3Tuple {
  return [x, -z, y];
}

/**
 * Project frame (X/Y horizontal, Z up, metres) -> three.js viewport frame
 * (Y up), centred on the scan: viewport = (x - cx, z - cz, -(y - cy)).
 */
export function toViewport(x: number, y: number, z: number, centerX: number, centerY: number, centerZ: number): Vec3Tuple {
  return [x - centerX, z - centerZ, -(y - centerY)];
}

export function toViewportPositions(source: Float32Array, centerX: number, centerY: number, centerZ: number): Float32Array {
  const output = new Float32Array(source.length);
  for (let index = 0; index + 2 < source.length; index += 3) {
    const transformed = toViewport(source[index], source[index + 1], source[index + 2], centerX, centerY, centerZ);
    output[index] = transformed[0]; output[index + 1] = transformed[1]; output[index + 2] = transformed[2];
  }
  return output;
}

export function splitConfidenceBands(positions: Float32Array, confidence: Float32Array, centerX: number, centerY: number, centerZ: number): QualityBands {
  const count = Math.min(Math.floor(positions.length / 3), confidence.length);
  let weakCount = 0;
  let improvingCount = 0;
  let stableCount = 0;
  for (let index = 0; index < count; index += 1) {
    if (confidence[index] < WEAK_CONFIDENCE) weakCount += 1;
    else if (confidence[index] < STABLE_CONFIDENCE) improvingCount += 1;
    else stableCount += 1;
  }
  const weak = new Float32Array(weakCount * 3);
  const improving = new Float32Array(improvingCount * 3);
  const stable = new Float32Array(stableCount * 3);
  let weakWrite = 0;
  let improvingWrite = 0;
  let stableWrite = 0;
  for (let index = 0; index < count; index += 1) {
    const offset = index * 3;
    const transformed = toViewport(positions[offset], positions[offset + 1], positions[offset + 2], centerX, centerY, centerZ);
    const target = confidence[index] < WEAK_CONFIDENCE ? weak
      : confidence[index] < STABLE_CONFIDENCE ? improving
        : stable;
    const write = target === weak ? weakWrite : target === improving ? improvingWrite : stableWrite;
    target[write] = transformed[0];
    target[write + 1] = transformed[1];
    target[write + 2] = transformed[2];
    if (target === weak) weakWrite += 3;
    else if (target === improving) improvingWrite += 3;
    else stableWrite += 3;
  }
  return { weak, improving, stable };
}

const BOX_EDGES: ReadonlyArray<readonly [number, number]> = [[0, 1], [0, 2], [0, 4], [1, 3], [1, 5], [2, 3], [2, 6], [3, 7], [4, 5], [4, 6], [5, 7], [6, 7]];

export function boundsLinePositions(snapshot: Pick<ScanPointCloudSnapshot, 'bounds'>, centerX: number, centerY: number, centerZ: number): Float32Array {
  const bounds = snapshot.bounds;
  if (bounds === null) return new Float32Array(0);
  const corners: Vec3Tuple[] = [];
  for (const x of [bounds.min[0], bounds.max[0]]) for (const y of [bounds.min[1], bounds.max[1]]) for (const z of [bounds.min[2], bounds.max[2]]) corners.push(toViewport(x, y, z, centerX, centerY, centerZ));
  const output = new Float32Array(BOX_EDGES.length * 6); let write = 0;
  for (const [a, b] of BOX_EDGES) for (const pointIndex of [a, b]) { const point = corners[pointIndex]; output[write++] = point[0]; output[write++] = point[1]; output[write++] = point[2]; }
  return output;
}

export function operatorEdgePositions(snapshot: Pick<ScanPointCloudSnapshot, 'operatorEdges'>, centerX: number, centerY: number, centerZ: number): Float32Array {
  const output = new Float32Array(snapshot.operatorEdges.length * 6);
  let write = 0;
  for (const edge of snapshot.operatorEdges) {
    for (const point of [edge.start, edge.end] as const) {
      const transformed = toViewport(point[0], point[1], point[2], centerX, centerY, centerZ);
      output[write++] = transformed[0]; output[write++] = transformed[1]; output[write++] = transformed[2];
    }
  }
  return output;
}

export type ScanSceneSource = Pick<ScanPointCloudSnapshot,
  'bounds' | 'positions' | 'confidence' | 'tentativePositions' | 'tentativeConfidence' |
  'manualControlPoints' | 'operatorEdges' | 'freeAirPositions' | 'unknownCandidatePositions'>;

function scanSceneMetrics(snapshot: ScanSceneSource) {
  const bounds = snapshot.bounds;
  const centerX = bounds === null ? 0 : (bounds.min[0] + bounds.max[0]) * 0.5;
  const centerY = bounds === null ? 0 : (bounds.min[1] + bounds.max[1]) * 0.5;
  const centerZ = bounds === null ? 0 : (bounds.min[2] + bounds.max[2]) * 0.5;
  const extentX = bounds === null ? 0 : bounds.max[0] - bounds.min[0];
  const extentY = bounds === null ? 0 : bounds.max[1] - bounds.min[1];
  const extentZ = bounds === null ? 0 : bounds.max[2] - bounds.min[2];
  const extent = Math.max(0.8, extentX, extentY, extentZ);
  return { centerX, centerY, centerZ, extent, extentX, extentY, extentZ };
}

export function prepareScanScene(
  snapshot: ScanSceneSource,
  needs: ScanSceneMaterializationNeeds = ALL_MATERIALIZATION_NEEDS,
): PreparedScanScene {
  const metrics = scanSceneMetrics(snapshot);
  return {
    good: needs.good ? splitConfidenceBands(snapshot.positions, snapshot.confidence, metrics.centerX, metrics.centerY, metrics.centerZ) : EMPTY_BANDS,
    bad: needs.bad ? splitConfidenceBands(snapshot.tentativePositions, snapshot.tentativeConfidence, metrics.centerX, metrics.centerY, metrics.centerZ) : EMPTY_BANDS,
    manualPositions: needs.manualPoints
      ? snapshot.manualControlPoints.map((point) => toViewport(point.position[0], point.position[1], point.position[2], metrics.centerX, metrics.centerY, metrics.centerZ))
      : [],
    operatorEdgePositions: needs.manualPoints ? operatorEdgePositions(snapshot, metrics.centerX, metrics.centerY, metrics.centerZ) : EMPTY_POSITIONS,
    freeAirPositions: needs.freeAir ? toViewportPositions(snapshot.freeAirPositions, metrics.centerX, metrics.centerY, metrics.centerZ) : EMPTY_POSITIONS,
    unknownPositions: needs.unknownSpace ? toViewportPositions(snapshot.unknownCandidatePositions, metrics.centerX, metrics.centerY, metrics.centerZ) : EMPTY_POSITIONS,
    boundsLines: needs.dimensions ? boundsLinePositions(snapshot, metrics.centerX, metrics.centerY, metrics.centerZ) : EMPTY_POSITIONS,
    ...metrics,
  };
}

/** Reuses requested viewport buffers and releases them when the scan generation changes. */
export class ScanSceneMaterializer {
  private generation: string | number | object | null = null;
  private snapshot: ScanSceneSource | null = null;
  private metrics: ReturnType<typeof scanSceneMetrics> | null = null;
  private good: QualityBands | null = null;
  private bad: QualityBands | null = null;
  private freeAir: Float32Array | null = null;
  private unknown: Float32Array | null = null;
  private manualPositions: ReadonlyArray<Vec3Tuple> | null = null;
  private operatorEdges: Float32Array | null = null;
  private boundsLines: Float32Array | null = null;

  prepare(
    snapshot: ScanSceneSource,
    generation: string | number | object,
    needs: ScanSceneMaterializationNeeds,
  ): PreparedScanScene {
    if (this.snapshot !== snapshot || this.generation !== generation) {
      this.release();
      this.snapshot = snapshot;
      this.generation = generation;
      this.metrics = scanSceneMetrics(snapshot);
    }
    const metrics = this.metrics ?? scanSceneMetrics(snapshot);
    if (needs.good && this.good === null) this.good = splitConfidenceBands(snapshot.positions, snapshot.confidence, metrics.centerX, metrics.centerY, metrics.centerZ);
    if (needs.bad && this.bad === null) this.bad = splitConfidenceBands(snapshot.tentativePositions, snapshot.tentativeConfidence, metrics.centerX, metrics.centerY, metrics.centerZ);
    if (needs.freeAir && this.freeAir === null) this.freeAir = toViewportPositions(snapshot.freeAirPositions, metrics.centerX, metrics.centerY, metrics.centerZ);
    if (needs.unknownSpace && this.unknown === null) this.unknown = toViewportPositions(snapshot.unknownCandidatePositions, metrics.centerX, metrics.centerY, metrics.centerZ);
    if (needs.manualPoints && this.manualPositions === null) {
      this.manualPositions = snapshot.manualControlPoints.map((point) => toViewport(point.position[0], point.position[1], point.position[2], metrics.centerX, metrics.centerY, metrics.centerZ));
      this.operatorEdges = operatorEdgePositions(snapshot, metrics.centerX, metrics.centerY, metrics.centerZ);
    }
    if (needs.dimensions && this.boundsLines === null) this.boundsLines = boundsLinePositions(snapshot, metrics.centerX, metrics.centerY, metrics.centerZ);
    return {
      good: this.good ?? EMPTY_BANDS,
      bad: this.bad ?? EMPTY_BANDS,
      manualPositions: this.manualPositions ?? [],
      operatorEdgePositions: this.operatorEdges ?? EMPTY_POSITIONS,
      freeAirPositions: this.freeAir ?? EMPTY_POSITIONS,
      unknownPositions: this.unknown ?? EMPTY_POSITIONS,
      boundsLines: this.boundsLines ?? EMPTY_POSITIONS,
      ...metrics,
    };
  }

  release(): void {
    this.generation = null;
    this.snapshot = null;
    this.metrics = null;
    this.good = null;
    this.bad = null;
    this.freeAir = null;
    this.unknown = null;
    this.manualPositions = null;
    this.operatorEdges = null;
    this.boundsLines = null;
  }
}

/** Camera radius that contains the surface, displayed source markers, uncertainty, and optional search depth. */
export function scanViewFitExtent(
  prepared: Pick<PreparedScanScene, 'centerX' | 'centerY' | 'centerZ' | 'extent'>,
  sourceHighlights: SourceHighlightScene | null,
  candidateVolume: SolverCandidateVolume | null = null,
  includeCandidateDepth = false,
): number {
  let extent = prepared.extent;
  const positions: Vec3Tuple[] = [];
  if (sourceHighlights !== null) {
    const primary = sourceHighlights.primary;
    if (primary !== null) positions.push(primary.voxel?.center ?? primary.edgeMarker ?? primary.positionProjectM);
    for (const source of sourceHighlights.secondary) positions.push(source.edgeMarker ?? source.positionProjectM);
    for (const cell of sourceHighlights.uncertaintyVoxels) positions.push(cell.center);
  }
  for (const position of positions) {
    extent = Math.max(extent,
      2 * Math.abs(position[0] - prepared.centerX),
      2 * Math.abs(position[1] - prepared.centerY),
      2 * Math.abs(position[2] - prepared.centerZ));
  }
  const candidateMinZ = candidateVolume?.min?.[2];
  if (includeCandidateDepth && candidateMinZ !== undefined) {
    extent = Math.max(extent, 2 * Math.abs(candidateMinZ - prepared.centerZ));
  }
  return Math.max(0.8, extent);
}

/* ---- Layer plan ---------------------------------------------------------- */

/** What kind of scanned information a layer shows. */
export type ScanLayerDataType =
  | 'unknown-space'
  | 'free-air'
  | 'surface-mesh'
  | 'tentative-surface'
  | 'stable-surface'
  | 'operator-edges'
  | 'operator-pins'
  | 'scan-bounds'
  | 'sensor-markers'
  | 'source-highlight';

export type ScanQualityBand = 'weak' | 'improving' | 'stable';

interface LayerBase {
  id: string;
  dataType: ScanLayerDataType;
  /** The display toggle that controls this layer. */
  toggle?: keyof ScanDisplayOptions;
  renderOrder: number;
}

export interface PointLayerSpec extends LayerBase {
  kind: 'points';
  positions: Float32Array;
  color: string;
  size: number;
  opacity: number;
  depthTest: boolean;
  band?: ScanQualityBand;
}

export interface MeshLayerSpec extends LayerBase {
  kind: 'mesh';
  positions: Float32Array;
  wireframe: boolean;
}

export interface LineLayerSpec extends LayerBase {
  kind: 'lines';
  positions: Float32Array;
  color: string;
  opacity: number;
}

export interface PinLayerSpec extends LayerBase {
  kind: 'pins';
  pins: ReadonlyArray<Vec3Tuple>;
}

export interface SensorMarkerLayerSpec extends LayerBase {
  kind: 'sensor-markers';
  markers: ReadonlyArray<{ marker: SensorPlacementMarker; positionViewport: Vec3Tuple }>;
}

export interface SourceHighlightLayerSpec extends LayerBase {
  kind: 'source-highlights';
  primaryVoxel: Vec3Tuple | null;
  uncertaintyVoxels: ReadonlyArray<Vec3Tuple>;
  voxelSizeMeters: number;
  markers: ReadonlyArray<{ positionViewport: Vec3Tuple; primary: boolean; edge: boolean }>;
}

export type ScanLayerSpec = PointLayerSpec | MeshLayerSpec | LineLayerSpec | PinLayerSpec | SensorMarkerLayerSpec | SourceHighlightLayerSpec;

export const SCAN_LAYER_COLORS = {
  unknownSpace: uiTheme.muted,
  freeAir: uiTheme.linkMeasuring,
  weak: uiTheme.error,
  improving: uiTheme.warning,
  stable: uiTheme.good,
  operatorEdges: uiTheme.accent,
  bounds: uiTheme.text,
} as const;

function qualityBandLayers(
  bands: QualityBands,
  extent: number,
  faint: boolean,
  dataType: 'tentative-surface' | 'stable-surface',
  toggle: 'badPoints' | 'goodPoints',
): PointLayerSpec[] {
  const base = clamp(extent * 0.008, 0.014, 0.045);
  const bandSpec: ReadonlyArray<readonly [ScanQualityBand, string, number, number, number]> = [
    ['weak', SCAN_LAYER_COLORS.weak, 0.86, faint ? 0.42 : 0.62, 0],
    ['improving', SCAN_LAYER_COLORS.improving, 1, faint ? 0.58 : 0.82, 0],
    ['stable', SCAN_LAYER_COLORS.stable, 1.12, faint ? 0.76 : 0.98, 0],
  ];
  return bandSpec.map(([band, color, sizeScale, opacity, renderOrder]) => ({
    kind: 'points',
    id: `${dataType}-${band}`,
    dataType,
    toggle,
    band,
    positions: bands[band],
    color,
    size: base * sizeScale,
    opacity,
    depthTest: true,
    renderOrder,
  }));
}

/**
 * The layers the viewport draws for `display`, in draw order. Empty layers
 * are omitted -- they draw nothing. The mesh overlay is wireframed only in
 * the dedicated SURFACE MESH mode.
 */
export function scanLayerPlan(
  prepared: PreparedScanScene,
  display: ScanDisplayOptions,
  meshPositions: Float32Array,
  wireframe: boolean,
  sensorMarkers: readonly SensorPlacementMarker[] = [],
  sourceHighlights: SourceHighlightScene | null = null,
): ScanLayerSpec[] {
  const voxelPointSize = clamp(prepared.extent * 0.010, 0.018, 0.055);
  const layers: ScanLayerSpec[] = [];
  if (display.unknownSpace) {
    layers.push({ kind: 'points', id: 'unknown-space', dataType: 'unknown-space', toggle: 'unknownSpace', positions: prepared.unknownPositions, color: SCAN_LAYER_COLORS.unknownSpace, size: voxelPointSize, opacity: 0.30, depthTest: false, renderOrder: 1 });
  }
  if (display.freeAir) {
    layers.push({ kind: 'points', id: 'free-air', dataType: 'free-air', toggle: 'freeAir', positions: prepared.freeAirPositions, color: SCAN_LAYER_COLORS.freeAir, size: voxelPointSize * 0.82, opacity: 0.48, depthTest: false, renderOrder: 2 });
  }
  if (display.mesh) {
    layers.push({ kind: 'mesh', id: 'surface-mesh', dataType: 'surface-mesh', toggle: 'mesh', positions: meshPositions, wireframe, renderOrder: 4 });
  }
  if (display.badPoints) layers.push(...qualityBandLayers(prepared.bad, prepared.extent, true, 'tentative-surface', 'badPoints'));
  if (display.goodPoints) layers.push(...qualityBandLayers(prepared.good, prepared.extent, false, 'stable-surface', 'goodPoints'));
  if (display.manualPoints) {
    layers.push({ kind: 'lines', id: 'operator-edges', dataType: 'operator-edges', toggle: 'manualPoints', positions: prepared.operatorEdgePositions, color: SCAN_LAYER_COLORS.operatorEdges, opacity: 0.96, renderOrder: 48 });
    layers.push({ kind: 'pins', id: 'operator-pins', dataType: 'operator-pins', toggle: 'manualPoints', pins: prepared.manualPositions, renderOrder: 50 });
  }
  if (display.dimensions) {
    layers.push({ kind: 'lines', id: 'scan-bounds', dataType: 'scan-bounds', toggle: 'dimensions', positions: prepared.boundsLines, color: SCAN_LAYER_COLORS.bounds, opacity: 0.78, renderOrder: 20 });
  }
  const placedMarkers = sensorMarkers.flatMap((marker) => marker.positionScanM === null ? [] : [{
    marker,
    positionViewport: toViewport(...marker.positionScanM, prepared.centerX, prepared.centerY, prepared.centerZ),
  }]);
  if (placedMarkers.length > 0) layers.push({ kind: 'sensor-markers', id: 'sensor-markers', dataType: 'sensor-markers', renderOrder: 60, markers: placedMarkers });
  if (sourceHighlights !== null) {
    const primaryVoxel = sourceHighlights.primary?.voxel === null || sourceHighlights.primary === null
      ? null
      : toViewport(...sourceHighlights.primary.voxel.center, prepared.centerX, prepared.centerY, prepared.centerZ);
    const markers = [
      ...(sourceHighlights.primary?.edgeMarker === null || sourceHighlights.primary?.edgeMarker === undefined ? [] : [{
        positionViewport: toViewport(...sourceHighlights.primary.edgeMarker, prepared.centerX, prepared.centerY, prepared.centerZ),
        primary: true,
        edge: true,
      }]),
      ...sourceHighlights.secondary.map((source) => ({
        positionViewport: toViewport(...(source.edgeMarker ?? source.positionProjectM), prepared.centerX, prepared.centerY, prepared.centerZ),
        primary: false,
        edge: source.edgeMarker !== null,
      })),
    ];
    if (primaryVoxel !== null || sourceHighlights.uncertaintyVoxels.length > 0 || markers.length > 0) {
      layers.push({
        kind: 'source-highlights', id: 'source-highlight', dataType: 'source-highlight', renderOrder: 80,
        primaryVoxel,
        uncertaintyVoxels: sourceHighlights.uncertaintyVoxels.map((cell) =>
          toViewport(...cell.center, prepared.centerX, prepared.centerY, prepared.centerZ)),
        voxelSizeMeters: sourceHighlights.voxelSizeMeters,
        markers,
      });
    }
  }
  return layers.filter((layer) => layer.kind === 'pins'
    ? layer.pins.length > 0
    : layer.kind === 'sensor-markers' ? layer.markers.length > 0
      : layer.kind === 'source-highlights' ? layer.primaryVoxel !== null || layer.uncertaintyVoxels.length > 0 || layer.markers.length > 0
        : layer.positions.length > 0);
}

/* ---- Camera --------------------------------------------------------------- */

export interface CameraPose {
  position: Vec3Tuple;
  up: Vec3Tuple;
  target: Vec3Tuple;
}

/** Where the 3D / Top / Front buttons put the camera, scaled to the scan. */
export function cameraPoseForPreset(preset: ViewportPreset, extent: number): CameraPose {
  const distance = Math.max(1.5, extent * 1.75);
  const target: Vec3Tuple = [0, 0, 0];
  if (preset === 'top') return { position: [0, distance, 0.001], up: [0, 0, -1], target };
  if (preset === 'front') return { position: [0, extent * 0.08, distance], up: [0, 1, 0], target };
  return { position: [distance * 0.66, distance * 0.52, distance * 0.72], up: [0, 1, 0], target };
}

/**
 * Whether the camera is put back to a preset pose. An explicit view command (3D / Top / Front / Reset) always does. A change of the fitted
 * extent alone (a layer toggled, candidate depth or source overlay changed the bounds) re-fits only while the operator has not moved the
 * camera; afterwards it only updates the zoom limits, so a pan or rotation is never undone behind their back (issue #41).
 */
export function shouldRefitCamera(input: { commandChanged: boolean; operatorMovedCamera: boolean }): boolean {
  return input.commandChanged || !input.operatorMovedCamera;
}

/** A camera command from the viewport's own control strip (fit the scan, reset the view, focus the selected sensor). */
export interface CameraHudCommand {
  kind: 'fit' | 'reset' | 'focus';
  sequence: number;
  /** Viewport-frame point to focus (kind 'focus' only). */
  target: Vec3Tuple | null;
}

/**
 * Where the camera goes when the operator focuses a point: it keeps looking from the same direction, only the orbit centre and the distance
 * change (about half the scan extent, never closer than 0.6 m nor farther than 12 m). Camera only: no scan or sensor value is touched.
 */
export function focusPoseForTarget(input: {
  cameraPosition: Vec3Tuple; previousTarget: Vec3Tuple; target: Vec3Tuple; extent: number;
}): { position: Vec3Tuple; target: Vec3Tuple } {
  const [cx, cy, cz] = input.cameraPosition;
  const [px, py, pz] = input.previousTarget;
  let dx = cx - px, dy = cy - py, dz = cz - pz;
  let length = Math.hypot(dx, dy, dz);
  if (!Number.isFinite(length) || length < 1e-6) { dx = 0.66; dy = 0.52; dz = 0.72; length = Math.hypot(dx, dy, dz); }
  const extent = Number.isFinite(input.extent) ? input.extent : 0;
  const distance = Math.min(12, Math.max(0.6, extent * 0.5));
  const k = distance / length;
  const [tx, ty, tz] = input.target;
  return { position: [tx + dx * k, ty + dy * k, tz + dz * k], target: [tx, ty, tz] };
}

/** What one finger / the left mouse button does: rotate by default (recommended), or move the view in Pan mode. Two fingers always move and zoom. */
export function dragModeForPan(panMode: boolean): 'rotate' | 'pan' {
  return panMode ? 'pan' : 'rotate';
}

export function orbitDistanceLimits(extent: number): { minDistance: number; maxDistance: number } {
  return { minDistance: Math.max(0.18, extent * 0.10), maxDistance: Math.max(18, extent * 10) };
}

/* ---- Overlay text ---------------------------------------------------------- */

export function scanSourceBadgeText(snapshot: Pick<ScanPointCloudSnapshot,
  'pointCount' | 'knownFreeVoxelCount' | 'unknownCandidateVoxelCount' | 'manualControlPoints' | 'operatorEdges'>, locale: I18nLocale = 'en'): string {
  return `${translate(locale, 'scan.badge.surface')} ${formatNumber(locale, snapshot.pointCount)} · ${translate(locale, 'scan.badge.free')} ${formatNumber(locale, snapshot.knownFreeVoxelCount)} · ${translate(locale, 'scan.badge.unknown')} ${formatNumber(locale, snapshot.unknownCandidateVoxelCount)} · ${translate(locale, 'scan.badge.pins')} ${formatNumber(locale, snapshot.manualControlPoints.length)} · ${translate(locale, 'scan.badge.edges')} ${formatNumber(locale, snapshot.operatorEdges.length)}`;
}

export function scanDimensionsText(prepared: Pick<PreparedScanScene, 'extentX' | 'extentY' | 'extentZ'>, locale: I18nLocale = 'en'): string {
  return `X ${formatDistance(locale, prepared.extentX)} · Y ${formatDistance(locale, prepared.extentY)} · Z ${formatDistance(locale, prepared.extentZ)}`;
}
