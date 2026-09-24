import type { ScanDisplayOptions } from './scanDisplayModes';
import type { ScanPointCloudSnapshot } from './webxrRuntime';

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

/** Confidence below this is WEAK, below STABLE_CONFIDENCE is IMPROVING. */
export const WEAK_CONFIDENCE = 0.42;
export const STABLE_CONFIDENCE = 0.72;

export function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
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
  const weak: number[] = []; const improving: number[] = []; const stable: number[] = [];
  const count = Math.min(Math.floor(positions.length / 3), confidence.length);
  for (let index = 0; index < count; index += 1) {
    const offset = index * 3;
    const transformed = toViewport(positions[offset], positions[offset + 1], positions[offset + 2], centerX, centerY, centerZ);
    const target = confidence[index] < WEAK_CONFIDENCE ? weak : confidence[index] < STABLE_CONFIDENCE ? improving : stable;
    target.push(transformed[0], transformed[1], transformed[2]);
  }
  return { weak: Float32Array.from(weak), improving: Float32Array.from(improving), stable: Float32Array.from(stable) };
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

export function prepareScanScene(snapshot: ScanSceneSource): PreparedScanScene {
  const bounds = snapshot.bounds;
  const centerX = bounds === null ? 0 : (bounds.min[0] + bounds.max[0]) * 0.5;
  const centerY = bounds === null ? 0 : (bounds.min[1] + bounds.max[1]) * 0.5;
  const centerZ = bounds === null ? 0 : (bounds.min[2] + bounds.max[2]) * 0.5;
  const extentX = bounds === null ? 0 : bounds.max[0] - bounds.min[0];
  const extentY = bounds === null ? 0 : bounds.max[1] - bounds.min[1];
  const extentZ = bounds === null ? 0 : bounds.max[2] - bounds.min[2];
  const extent = Math.max(0.8, extentX, extentY, extentZ);
  return {
    good: splitConfidenceBands(snapshot.positions, snapshot.confidence, centerX, centerY, centerZ),
    bad: splitConfidenceBands(snapshot.tentativePositions, snapshot.tentativeConfidence, centerX, centerY, centerZ),
    manualPositions: snapshot.manualControlPoints.map((point) => toViewport(point.position[0], point.position[1], point.position[2], centerX, centerY, centerZ)),
    operatorEdgePositions: operatorEdgePositions(snapshot, centerX, centerY, centerZ),
    freeAirPositions: toViewportPositions(snapshot.freeAirPositions, centerX, centerY, centerZ),
    unknownPositions: toViewportPositions(snapshot.unknownCandidatePositions, centerX, centerY, centerZ),
    boundsLines: boundsLinePositions(snapshot, centerX, centerY, centerZ),
    centerX, centerY, centerZ, extent, extentX, extentY, extentZ,
  };
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
  | 'scan-bounds';

export type ScanQualityBand = 'weak' | 'improving' | 'stable';

interface LayerBase {
  id: string;
  dataType: ScanLayerDataType;
  /** The display toggle that controls this layer. */
  toggle: keyof ScanDisplayOptions;
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

export type ScanLayerSpec = PointLayerSpec | MeshLayerSpec | LineLayerSpec | PinLayerSpec;

export const SCAN_LAYER_COLORS = {
  unknownSpace: '#f59e0b',
  freeAir: '#38bdf8',
  weak: '#ff3d28',
  improving: '#facc15',
  stable: '#2ee6a6',
  operatorEdges: '#f05cff',
  bounds: '#dbeafe',
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
  return layers.filter((layer) => (layer.kind === 'pins' ? layer.pins.length : layer.positions.length) > 0);
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

export function orbitDistanceLimits(extent: number): { minDistance: number; maxDistance: number } {
  return { minDistance: Math.max(0.18, extent * 0.10), maxDistance: Math.max(18, extent * 10) };
}

/* ---- Overlay text ---------------------------------------------------------- */

export function scanSourceBadgeText(snapshot: Pick<ScanPointCloudSnapshot,
  'pointCount' | 'knownFreeVoxelCount' | 'unknownCandidateVoxelCount' | 'manualControlPoints' | 'operatorEdges'>): string {
  return `SURFACE ${snapshot.pointCount.toLocaleString()} · FREE ${snapshot.knownFreeVoxelCount.toLocaleString()} · UNKNOWN ${snapshot.unknownCandidateVoxelCount.toLocaleString()} · PINS ${snapshot.manualControlPoints.length.toLocaleString()} · EDGES ${snapshot.operatorEdges.length.toLocaleString()}`;
}

export function scanDimensionsText(prepared: Pick<PreparedScanScene, 'extentX' | 'extentY' | 'extentZ'>): string {
  return `X ${prepared.extentX.toFixed(2)} m · Y ${prepared.extentY.toFixed(2)} m · Z ${prepared.extentZ.toFixed(2)} m`;
}
