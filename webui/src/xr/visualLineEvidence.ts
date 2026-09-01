import type { RgbDepthGuidanceFrame } from './rgbDepthGuidance';

export type NormalizedPoint2 = readonly [number, number];

export interface VisualLineSegment {
  id: string;
  start: NormalizedPoint2;
  end: NormalizedPoint2;
  direction: NormalizedPoint2;
  length: number;
  strength: number;
  supportPixels: number;
}

export interface VisualJunction {
  position: NormalizedPoint2;
  strength: number;
  lineAId: string;
  lineBId: string;
}

export interface VisualEvidenceFrame {
  keyframeId: number;
  capturedAtMs: number;
  lines: readonly VisualLineSegment[];
  junctions: readonly VisualJunction[];
  projectionMatrix: Float32Array;
  worldFromView: Float32Array;
}

export interface DetectVisualLinesOptions {
  edgeThreshold?: number;
  angleBins?: number;
  rhoBins?: number;
  minimumVotes?: number;
  minimumLength?: number;
  maxLines?: number;
}

interface HoughBin {
  votes: number;
  weightedVotes: number;
  pixels: number[];
  theta: number;
  rho: number;
}

let deferredToWorker = false;

/** Production WebXR enables this through xrGuidanceRegistry. Worker/test modules do not. */
export function setVisualLineDetectionDeferred(value: boolean): void {
  deferredToWorker = value;
}

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}
function clamp01(value: number): number { return clamp(value, 0, 1); }
function canonicalDirection(direction: NormalizedPoint2): NormalizedPoint2 {
  return direction[0] < -1e-8 || (Math.abs(direction[0]) <= 1e-8 && direction[1] < 0)
    ? [-direction[0], -direction[1]]
    : direction;
}
function pointSegmentDistance(point: NormalizedPoint2, line: VisualLineSegment): number {
  const dx = line.end[0] - line.start[0];
  const dy = line.end[1] - line.start[1];
  const squared = dx * dx + dy * dy;
  if (squared < 1e-12) return Math.hypot(point[0] - line.start[0], point[1] - line.start[1]);
  const t = clamp(((point[0] - line.start[0]) * dx + (point[1] - line.start[1]) * dy) / squared, 0, 1);
  return Math.hypot(point[0] - (line.start[0] + dx * t), point[1] - (line.start[1] + dy * t));
}
function lineIntersection(a: VisualLineSegment, b: VisualLineSegment): NormalizedPoint2 | null {
  const ax = a.start[0], ay = a.start[1], bx = b.start[0], by = b.start[1];
  const adx = a.end[0] - ax, ady = a.end[1] - ay;
  const bdx = b.end[0] - bx, bdy = b.end[1] - by;
  const determinant = adx * bdy - ady * bdx;
  if (Math.abs(determinant) < 1e-6) return null;
  const rx = bx - ax, ry = by - ay;
  const ta = (rx * bdy - ry * bdx) / determinant;
  return [ax + adx * ta, ay + ady * ta];
}

/**
 * Immediate bounded Hough-style detector. This is the implementation used by
 * the worker and isolated tests. Production XR calls detectVisualLines(), which
 * is switched to a no-op handoff marker so the hot frame callback stays cheap.
 */
export function detectVisualLinesNow(
  frame: RgbDepthGuidanceFrame,
  options: DetectVisualLinesOptions = {},
): { lines: VisualLineSegment[]; junctions: VisualJunction[] } {
  const edgeThreshold = options.edgeThreshold ?? 0.26;
  const angleBins = Math.max(8, Math.floor(options.angleBins ?? 18));
  const rhoBins = Math.max(24, Math.floor(options.rhoBins ?? 72));
  const minimumVotes = Math.max(5, Math.floor(options.minimumVotes ?? Math.max(7, Math.min(frame.width, frame.height) * 0.035)));
  const minimumLength = options.minimumLength ?? 0.075;
  const maxLines = Math.max(1, Math.floor(options.maxLines ?? 24));
  const diagonal = Math.SQRT2;
  const bins = new Map<number, HoughBin>();

  for (let y = 1; y < frame.height - 1; y += 1) {
    for (let x = 1; x < frame.width - 1; x += 1) {
      const index = y * frame.width + x;
      const strength = frame.edgeStrength[index] / 255;
      if (strength < edgeThreshold) continue;
      const gx = frame.gradientX[index] / 127;
      const gy = frame.gradientY[index] / 127;
      if (Math.hypot(gx, gy) < 0.15) continue;
      let theta = Math.atan2(gy, gx);
      if (theta < 0) theta += Math.PI;
      if (theta >= Math.PI) theta -= Math.PI;
      const angleIndex = clamp(Math.floor((theta / Math.PI) * angleBins), 0, angleBins - 1);
      const quantizedTheta = ((angleIndex + 0.5) / angleBins) * Math.PI;
      const u = (x + 0.5) / frame.width;
      const v = (y + 0.5) / frame.height;
      const rho = u * Math.cos(quantizedTheta) + v * Math.sin(quantizedTheta);
      const rhoIndex = clamp(Math.floor(((rho + diagonal) / (2 * diagonal)) * rhoBins), 0, rhoBins - 1);
      const key = angleIndex * rhoBins + rhoIndex;
      const existing = bins.get(key) ?? { votes: 0, weightedVotes: 0, pixels: [], theta: quantizedTheta, rho };
      existing.votes += 1;
      existing.weightedVotes += strength;
      if (existing.pixels.length < 512) existing.pixels.push(index);
      bins.set(key, existing);
    }
  }

  const raw: VisualLineSegment[] = [];
  for (const bin of bins.values()) {
    if (bin.votes < minimumVotes) continue;
    const tangent = canonicalDirection([-Math.sin(bin.theta), Math.cos(bin.theta)]);
    let minimum = Number.POSITIVE_INFINITY, maximum = Number.NEGATIVE_INFINITY;
    let meanU = 0, meanV = 0, weightSum = 0;
    for (const pixel of bin.pixels) {
      const x = pixel % frame.width;
      const y = Math.floor(pixel / frame.width);
      const u = (x + 0.5) / frame.width;
      const v = (y + 0.5) / frame.height;
      const weight = Math.max(0.05, frame.edgeStrength[pixel] / 255);
      const parameter = u * tangent[0] + v * tangent[1];
      minimum = Math.min(minimum, parameter);
      maximum = Math.max(maximum, parameter);
      meanU += u * weight;
      meanV += v * weight;
      weightSum += weight;
    }
    if (!Number.isFinite(minimum) || !Number.isFinite(maximum) || weightSum <= 0) continue;
    if (maximum - minimum < minimumLength) continue;
    const center: NormalizedPoint2 = [meanU / weightSum, meanV / weightSum];
    const centerParameter = center[0] * tangent[0] + center[1] * tangent[1];
    const start: NormalizedPoint2 = [
      clamp01(center[0] + tangent[0] * (minimum - centerParameter)),
      clamp01(center[1] + tangent[1] * (minimum - centerParameter)),
    ];
    const end: NormalizedPoint2 = [
      clamp01(center[0] + tangent[0] * (maximum - centerParameter)),
      clamp01(center[1] + tangent[1] * (maximum - centerParameter)),
    ];
    raw.push({
      id: '',
      start,
      end,
      direction: tangent,
      length: Math.hypot(end[0] - start[0], end[1] - start[1]),
      strength: clamp01(bin.weightedVotes / Math.max(1, bin.votes)),
      supportPixels: bin.votes,
    });
  }

  raw.sort((a, b) =>
    (b.strength * b.length * Math.log2(b.supportPixels + 1)) -
    (a.strength * a.length * Math.log2(a.supportPixels + 1))
  );
  const lines: VisualLineSegment[] = [];
  for (const candidate of raw) {
    const duplicate = lines.some((line) => {
      const directionDot = Math.abs(line.direction[0] * candidate.direction[0] + line.direction[1] * candidate.direction[1]);
      const midpoint: NormalizedPoint2 = [
        (candidate.start[0] + candidate.end[0]) * 0.5,
        (candidate.start[1] + candidate.end[1]) * 0.5,
      ];
      return directionDot > Math.cos((7 * Math.PI) / 180) && pointSegmentDistance(midpoint, line) < 0.018;
    });
    if (duplicate) continue;
    candidate.id = `line:${lines.length + 1}`;
    lines.push(candidate);
    if (lines.length >= maxLines) break;
  }

  const junctions: VisualJunction[] = [];
  for (let first = 0; first < lines.length; first += 1) {
    for (let second = first + 1; second < lines.length; second += 1) {
      const a = lines[first], b = lines[second];
      const cosine = Math.abs(a.direction[0] * b.direction[0] + a.direction[1] * b.direction[1]);
      if (cosine > Math.cos((24 * Math.PI) / 180)) continue;
      const position = lineIntersection(a, b);
      if (position === null || position[0] < -0.02 || position[0] > 1.02 || position[1] < -0.02 || position[1] > 1.02) continue;
      if (pointSegmentDistance(position, a) > 0.035 || pointSegmentDistance(position, b) > 0.035) continue;
      if (junctions.some((item) => Math.hypot(item.position[0] - position[0], item.position[1] - position[1]) < 0.025)) continue;
      junctions.push({
        position: [clamp01(position[0]), clamp01(position[1])],
        strength: clamp01(Math.min(a.strength, b.strength) * (1 - cosine * 0.25)),
        lineAId: a.id,
        lineBId: b.id,
      });
    }
  }
  return { lines, junctions: junctions.sort((a, b) => b.strength - a.strength).slice(0, 20) };
}

export function detectVisualLines(
  frame: RgbDepthGuidanceFrame,
  options: DetectVisualLinesOptions = {},
): { lines: VisualLineSegment[]; junctions: VisualJunction[] } {
  if (deferredToWorker) return { lines: [], junctions: [] };
  return detectVisualLinesNow(frame, options);
}

function cloneEvidenceFrame(frame: VisualEvidenceFrame): VisualEvidenceFrame {
  return {
    ...frame,
    projectionMatrix: frame.projectionMatrix.slice(),
    worldFromView: frame.worldFromView.slice(),
    lines: frame.lines.map((line) => ({
      ...line,
      start: [...line.start] as NormalizedPoint2,
      end: [...line.end] as NormalizedPoint2,
      direction: [...line.direction] as NormalizedPoint2,
    })),
    junctions: frame.junctions.map((junction) => ({
      ...junction,
      position: [...junction.position] as NormalizedPoint2,
    })),
  };
}

function cameraPosition(frame: VisualEvidenceFrame): readonly [number, number, number] {
  return [frame.worldFromView[12] ?? 0, frame.worldFromView[13] ?? 0, frame.worldFromView[14] ?? 0];
}

function cameraForward(frame: VisualEvidenceFrame): readonly [number, number, number] | null {
  const x = -(frame.worldFromView[8] ?? 0);
  const y = -(frame.worldFromView[9] ?? 0);
  const z = -(frame.worldFromView[10] ?? 0);
  const length = Math.hypot(x, y, z);
  return length > 1e-6 ? [x / length, y / length, z / length] : null;
}

function poseDistance(a: VisualEvidenceFrame, b: VisualEvidenceFrame): number {
  const ap = cameraPosition(a), bp = cameraPosition(b);
  const translation = Math.hypot(ap[0] - bp[0], ap[1] - bp[1], ap[2] - bp[2]);
  const af = cameraForward(a), bf = cameraForward(b);
  let angle = 0;
  if (af !== null && bf !== null) {
    const dot = clamp(af[0] * bf[0] + af[1] * bf[1] + af[2] * bf[2], -1, 1);
    angle = Math.acos(dot);
  }
  return translation / 0.08 + angle / ((10 * Math.PI) / 180);
}

function evidenceQuality(frame: VisualEvidenceFrame): number {
  let strongest = 0;
  for (const line of frame.lines) strongest = Math.max(strongest, line.strength);
  for (const junction of frame.junctions) strongest = Math.max(strongest, junction.strength);
  return strongest + Math.min(0.35, frame.lines.length * 0.012 + frame.junctions.length * 0.02);
}

/**
 * Bounded pose-diverse visual history. A simple FIFO window caused late corners
 * to lose the independent RGB viewpoints collected earlier in a scan. When the
 * reservoir is full we remove one member of the closest pose pair, preferring
 * the frame with weaker visual evidence. This keeps the scan-wide baseline
 * diversity needed by candidate-specific multi-view reprojection.
 */
export class VisualLineEvidenceStore {
  private frames: VisualEvidenceFrame[] = [];
  constructor(private readonly maxFrames = 64) {}
  reset(): void { this.frames = []; }

  add(frame: VisualEvidenceFrame): void {
    this.frames = this.frames.filter((item) => item.keyframeId !== frame.keyframeId);
    this.frames.push(cloneEvidenceFrame(frame));

    while (this.frames.length > Math.max(1, this.maxFrames)) {
      let first = 0, second = 1, closest = Number.POSITIVE_INFINITY;
      for (let left = 0; left < this.frames.length; left += 1) {
        for (let right = left + 1; right < this.frames.length; right += 1) {
          const distance = poseDistance(this.frames[left], this.frames[right]);
          if (distance < closest) { closest = distance; first = left; second = right; }
        }
      }
      const firstQuality = evidenceQuality(this.frames[first]);
      const secondQuality = evidenceQuality(this.frames[second]);
      const remove = Math.abs(firstQuality - secondQuality) > 0.04
        ? (firstQuality < secondQuality ? first : second)
        : (this.frames[first].keyframeId < this.frames[second].keyframeId ? first : second);
      this.frames.splice(remove, 1);
    }

    this.frames.sort((a, b) => a.keyframeId - b.keyframeId);
  }

  snapshot(): readonly VisualEvidenceFrame[] {
    return this.frames.map(cloneEvidenceFrame);
  }
  get size(): number { return this.frames.length; }
}