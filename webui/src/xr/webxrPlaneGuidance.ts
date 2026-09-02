export type StructuralPlaneOrientation = 'horizontal' | 'vertical' | null;
export type ProjectPoint = readonly [number, number, number];
export type ProjectVector = readonly [number, number, number];

export interface StructuralPlanePrior {
  /** Plane origin copied into the same project coordinates as fused surfels. */
  center: ProjectPoint;
  /** Unit plane normal in project coordinates. */
  normal: ProjectVector;
  /** Conservative radius of the native polygon around its origin. */
  extentRadiusMeters: number;
  /** Polygon area in the plane-local X/Z coordinates. Diagnostic/advisory only. */
  areaSquareMeters: number;
  polygonVertexCount: number;
  orientation: StructuralPlaneOrientation;
  semanticLabel: string | null;
  lastChangedTime: number;
}

export interface XrPlanePointLike { x: number; y: number; z: number; }
export interface XrPlaneLike {
  planeSpace: object;
  polygon: readonly XrPlanePointLike[];
  orientation?: 'horizontal' | 'vertical' | null;
  semanticLabel?: string | null;
  lastChangedTime: number;
}
export interface XrPlanePoseLike { transform: { matrix: ArrayLike<number> } }
export interface XrPlaneFrameLike {
  detectedPlanes?: Iterable<XrPlaneLike>;
  getPose?(space: object, baseSpace: object): XrPlanePoseLike | null;
}

const MAX_CAPTURED_PLANES = 24;
const MIN_EXTENT_METERS = 0.035;
const MAX_EXTENT_METERS = 12;

function finite(value: number): boolean { return Number.isFinite(value); }
function projectPointFromReference(x: number, y: number, z: number): ProjectPoint {
  return [x, -z, y];
}
function projectVectorFromReference(x: number, y: number, z: number): ProjectVector | null {
  const px = x, py = -z, pz = y;
  const magnitude = Math.hypot(px, py, pz);
  return finite(magnitude) && magnitude > 1e-8 ? [px / magnitude, py / magnitude, pz / magnitude] : null;
}
function finiteMatrix16(matrix: ArrayLike<number>): boolean {
  if (matrix.length < 16) return false;
  for (let index = 0; index < 16; index += 1) if (!finite(Number(matrix[index]))) return false;
  return true;
}
function transformLocalPoint(matrix: ArrayLike<number>, point: XrPlanePointLike): ProjectPoint | null {
  const x = Number(point.x), y = Number(point.y), z = Number(point.z);
  if (!finite(x) || !finite(y) || !finite(z)) return null;
  const wx = Number(matrix[0]) * x + Number(matrix[4]) * y + Number(matrix[8]) * z + Number(matrix[12]);
  const wy = Number(matrix[1]) * x + Number(matrix[5]) * y + Number(matrix[9]) * z + Number(matrix[13]);
  const wz = Number(matrix[2]) * x + Number(matrix[6]) * y + Number(matrix[10]) * z + Number(matrix[14]);
  return finite(wx) && finite(wy) && finite(wz) ? projectPointFromReference(wx, wy, wz) : null;
}
function polygonAreaLocalXZ(polygon: readonly XrPlanePointLike[]): number {
  if (polygon.length < 3) return 0;
  let twiceArea = 0;
  for (let index = 0; index < polygon.length; index += 1) {
    const a = polygon[index], b = polygon[(index + 1) % polygon.length];
    if (![a.x, a.z, b.x, b.z].every(Number.isFinite)) return 0;
    twiceArea += a.x * b.z - b.x * a.z;
  }
  return Math.abs(twiceArea) * 0.5;
}

/**
 * Copy one native XRPlane while its XRFrame is active.
 *
 * PlaneSpace's local Y axis is the plane normal per the WebXR Plane Detection
 * specification. No XRPlane/XRSpace object escapes this function; only owned
 * numeric data survives the animation-frame callback.
 */
export function captureStructuralPlanePrior(
  plane: XrPlaneLike,
  frame: XrPlaneFrameLike,
  referenceSpace: object,
): StructuralPlanePrior | null {
  if (typeof frame.getPose !== 'function') return null;
  const pose = frame.getPose(plane.planeSpace, referenceSpace);
  if (pose === null || !finiteMatrix16(pose.transform.matrix)) return null;
  const matrix = pose.transform.matrix;
  const center = projectPointFromReference(Number(matrix[12]), Number(matrix[13]), Number(matrix[14]));
  // Column 1 is local +Y transformed into reference space for column-major XRRigidTransform matrices.
  const normal = projectVectorFromReference(Number(matrix[4]), Number(matrix[5]), Number(matrix[6]));
  if (normal === null || !center.every(Number.isFinite)) return null;

  let extentRadiusMeters = 0;
  let validVertices = 0;
  for (const vertex of plane.polygon) {
    const transformed = transformLocalPoint(matrix, vertex);
    if (transformed === null) continue;
    extentRadiusMeters = Math.max(
      extentRadiusMeters,
      Math.hypot(transformed[0] - center[0], transformed[1] - center[1], transformed[2] - center[2]),
    );
    validVertices += 1;
  }
  if (!finite(extentRadiusMeters) || extentRadiusMeters < MIN_EXTENT_METERS || extentRadiusMeters > MAX_EXTENT_METERS) return null;

  const areaSquareMeters = polygonAreaLocalXZ(plane.polygon);
  return {
    center,
    normal,
    extentRadiusMeters,
    areaSquareMeters: finite(areaSquareMeters) ? Math.max(0, areaSquareMeters) : 0,
    polygonVertexCount: validVertices,
    orientation: plane.orientation === 'horizontal' || plane.orientation === 'vertical' ? plane.orientation : null,
    semanticLabel: typeof plane.semanticLabel === 'string' && plane.semanticLabel.length > 0 ? plane.semanticLabel : null,
    lastChangedTime: finite(Number(plane.lastChangedTime)) ? Number(plane.lastChangedTime) : 0,
  };
}

/** Bounded, allocation-owned snapshot suitable for later worker structured cloning. */
export function captureStructuralPlanePriors(
  frame: XrPlaneFrameLike,
  referenceSpace: object,
  maximumPlanes = MAX_CAPTURED_PLANES,
): readonly StructuralPlanePrior[] {
  if (frame.detectedPlanes === undefined || typeof frame.getPose !== 'function') return [];
  const captured: StructuralPlanePrior[] = [];
  try {
    for (const plane of frame.detectedPlanes) {
      const prior = captureStructuralPlanePrior(plane, frame, referenceSpace);
      if (prior !== null) captured.push(prior);
    }
  } catch {
    // detectedPlanes/getPose are frame-scoped WebXR APIs. A UA/context failure is
    // advisory and must never interrupt physical depth fusion.
    return [];
  }
  const cap = Number.isFinite(maximumPlanes) ? Math.max(1, Math.min(48, Math.floor(maximumPlanes))) : MAX_CAPTURED_PLANES;
  // Prefer larger physically meaningful planes when the native tracker exposes
  // many small fragments. Semantics are intentionally not used for geometry.
  captured.sort((a, b) => b.areaSquareMeters - a.areaSquareMeters || b.extentRadiusMeters - a.extentRadiusMeters);
  return captured.slice(0, cap);
}
