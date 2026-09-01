export interface RgbDepthGuidanceFrame {
  width: number;
  height: number;
  /** Top-left-origin 8-bit luminance. */
  luma: Uint8Array;
  /** Sobel gradient magnitude, normalized to 0..255. */
  edgeStrength: Uint8Array;
  /** Quantized normalized Sobel gradient components in -127..127. */
  gradientX: Int8Array;
  gradientY: Int8Array;
  capturedAtMs: number;
}

export interface PhysicalDepthSampler {
  width: number;
  height: number;
  getDepthInMeters(u: number, v: number): number;
}

export interface RgbGuidanceSample {
  luma: number;
  edgeStrength: number;
  gradientX: number;
  gradientY: number;
}

export interface GuidedDepthSample {
  depthMeters: number;
  /** True only when more than the centre physical depth sample contributed. */
  refined: boolean;
  physicalSamples: number;
  edgeStrength: number;
}

const MIN_VALID_DEPTH_METERS = 0.08;
const MAX_VALID_DEPTH_METERS = 12;
const SOBEL_MAX = Math.sqrt(2) * 1020;

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}

function clamp01(value: number): number {
  return clamp(value, 0, 1);
}

function validDepth(value: number): boolean {
  return Number.isFinite(value) && value >= MIN_VALID_DEPTH_METERS && value <= MAX_VALID_DEPTH_METERS;
}

function lumaIndex(frame: RgbDepthGuidanceFrame, u: number, v: number): number {
  const x = clamp(Math.round(clamp01(u) * (frame.width - 1)), 0, frame.width - 1);
  const y = clamp(Math.round(clamp01(v) * (frame.height - 1)), 0, frame.height - 1);
  return y * frame.width + x;
}

export function rgbaToLumaDownsampled(
  rgba: Uint8Array,
  sourceWidth: number,
  sourceHeight: number,
  targetWidth: number,
  targetHeight: number,
  sourceOrigin: 'top-left' | 'bottom-left' = 'top-left',
): Uint8Array {
  const width = Math.max(1, Math.floor(targetWidth));
  const height = Math.max(1, Math.floor(targetHeight));
  const result = new Uint8Array(width * height);
  if (sourceWidth <= 0 || sourceHeight <= 0 || rgba.length < sourceWidth * sourceHeight * 4) return result;

  for (let y = 0; y < height; y += 1) {
    const normalizedY = (y + 0.5) / height;
    const sourceYTop = clamp(Math.floor(normalizedY * sourceHeight), 0, sourceHeight - 1);
    const sy = sourceOrigin === 'bottom-left' ? sourceHeight - 1 - sourceYTop : sourceYTop;
    for (let x = 0; x < width; x += 1) {
      const normalizedX = (x + 0.5) / width;
      const sx = clamp(Math.floor(normalizedX * sourceWidth), 0, sourceWidth - 1);
      const offset = (sy * sourceWidth + sx) * 4;
      // Integer Rec.709-like luma approximation. RGB camera evidence is a guide,
      // never a metric source, so exact colour management is unnecessary here.
      result[y * width + x] = clamp(Math.round(
        rgba[offset] * 0.2126 + rgba[offset + 1] * 0.7152 + rgba[offset + 2] * 0.0722,
      ), 0, 255);
    }
  }
  return result;
}

export function buildRgbDepthGuidanceFrameFromLuma(
  luma: Uint8Array,
  width: number,
  height: number,
  capturedAtMs = performance.now(),
): RgbDepthGuidanceFrame {
  const w = Math.max(1, Math.floor(width));
  const h = Math.max(1, Math.floor(height));
  if (luma.length < w * h) throw new Error('RGB guidance luma buffer is smaller than width*height.');
  const normalizedLuma = luma.length === w * h ? luma : luma.slice(0, w * h);
  const edgeStrength = new Uint8Array(w * h);
  const gradientX = new Int8Array(w * h);
  const gradientY = new Int8Array(w * h);

  const at = (x: number, y: number): number => normalizedLuma[
    clamp(y, 0, h - 1) * w + clamp(x, 0, w - 1)
  ];

  for (let y = 0; y < h; y += 1) {
    for (let x = 0; x < w; x += 1) {
      const gx =
        -at(x - 1, y - 1) + at(x + 1, y - 1)
        - 2 * at(x - 1, y) + 2 * at(x + 1, y)
        - at(x - 1, y + 1) + at(x + 1, y + 1);
      const gy =
        -at(x - 1, y - 1) - 2 * at(x, y - 1) - at(x + 1, y - 1)
        + at(x - 1, y + 1) + 2 * at(x, y + 1) + at(x + 1, y + 1);
      const magnitude = Math.hypot(gx, gy);
      const index = y * w + x;
      edgeStrength[index] = clamp(Math.round((magnitude / SOBEL_MAX) * 255), 0, 255);
      if (magnitude > 1e-6) {
        gradientX[index] = clamp(Math.round((gx / magnitude) * 127), -127, 127);
        gradientY[index] = clamp(Math.round((gy / magnitude) * 127), -127, 127);
      }
    }
  }

  return { width: w, height: h, luma: normalizedLuma, edgeStrength, gradientX, gradientY, capturedAtMs };
}

export function buildRgbDepthGuidanceFrame(
  rgba: Uint8Array,
  sourceWidth: number,
  sourceHeight: number,
  targetWidth = 256,
  targetHeight = 144,
  sourceOrigin: 'top-left' | 'bottom-left' = 'top-left',
  capturedAtMs = performance.now(),
): RgbDepthGuidanceFrame {
  return buildRgbDepthGuidanceFrameFromLuma(
    rgbaToLumaDownsampled(rgba, sourceWidth, sourceHeight, targetWidth, targetHeight, sourceOrigin),
    targetWidth,
    targetHeight,
    capturedAtMs,
  );
}

export function sampleRgbGuidance(
  frame: RgbDepthGuidanceFrame | null | undefined,
  u: number,
  v: number,
): RgbGuidanceSample | null {
  if (frame === null || frame === undefined || frame.width <= 0 || frame.height <= 0) return null;
  const index = lumaIndex(frame, u, v);
  return {
    luma: frame.luma[index] / 255,
    edgeStrength: frame.edgeStrength[index] / 255,
    gradientX: frame.gradientX[index] / 127,
    gradientY: frame.gradientY[index] / 127,
  };
}

/**
 * Joint-bilateral-like refinement using ONLY physical depth measurements.
 * RGB changes weights and therefore preserves discontinuities, but it can never
 * create a depth value when the physical sensor has no valid sample nearby.
 */
export function guidedDepthInMeters(
  depth: PhysicalDepthSampler,
  u: number,
  v: number,
  guidance: RgbDepthGuidanceFrame | null | undefined,
): GuidedDepthSample | null {
  const centre = depth.getDepthInMeters(clamp01(u), clamp01(v));
  const centreValid = validDepth(centre);
  if (guidance === null || guidance === undefined) {
    return centreValid ? { depthMeters: centre, refined: false, physicalSamples: 1, edgeStrength: 0 } : null;
  }

  const guideCentre = sampleRgbGuidance(guidance, u, v);
  if (guideCentre === null) return centreValid ? { depthMeters: centre, refined: false, physicalSamples: 1, edgeStrength: 0 } : null;
  const du = 1 / Math.max(1, depth.width);
  const dv = 1 / Math.max(1, depth.height);
  let weightedDepth = 0;
  let weightSum = 0;
  let physicalSamples = 0;
  const spatialSigmaSquared = 0.95 * 0.95;
  const lumaSigma = guideCentre.edgeStrength >= 0.22 ? 0.055 : 0.11;

  for (let oy = -1; oy <= 1; oy += 1) {
    for (let ox = -1; ox <= 1; ox += 1) {
      const su = clamp01(u + ox * du);
      const sv = clamp01(v + oy * dv);
      const metric = depth.getDepthInMeters(su, sv);
      if (!validDepth(metric)) continue;
      const guide = sampleRgbGuidance(guidance, su, sv);
      if (guide === null) continue;
      const spatial = Math.exp(-(ox * ox + oy * oy) / (2 * spatialSigmaSquared));
      const lumaDelta = Math.abs(guide.luma - guideCentre.luma);
      const range = Math.exp(-(lumaDelta * lumaDelta) / (2 * lumaSigma * lumaSigma));
      let weight = spatial * range;
      if (ox === 0 && oy === 0) weight *= 1.35;
      weightedDepth += metric * weight;
      weightSum += weight;
      physicalSamples += 1;
    }
  }

  if (weightSum <= 1e-8 || physicalSamples === 0) return null;
  const refined = weightedDepth / weightSum;
  // At a strong RGB boundary, do not let interpolation pull a known foreground
  // sample toward a different surface. The centre physical measurement wins if
  // disagreement is larger than a conservative sensor-scale tolerance.
  if (
    centreValid && guideCentre.edgeStrength >= 0.24 &&
    Math.abs(refined - centre) > Math.max(0.035, 0.025 * centre)
  ) {
    return {
      depthMeters: centre,
      refined: false,
      physicalSamples,
      edgeStrength: guideCentre.edgeStrength,
    };
  }
  return {
    depthMeters: refined,
    refined: physicalSamples > 1 && (!centreValid || Math.abs(refined - centre) > 1e-4),
    physicalSamples,
    edgeStrength: guideCentre.edgeStrength,
  };
}

/**
 * Evidence weight for a physical depth sample at a detected depth discontinuity.
 * A coincident strong RGB edge is a classic mixed/flying-pixel risk, so the
 * centre sample is kept only as weak boundary evidence while side bands carry
 * the plane-fitting evidence.
 */
export function guidanceBoundaryWeight(
  depthEdge: boolean,
  guidance: RgbDepthGuidanceFrame | null | undefined,
  u: number,
  v: number,
): number {
  if (!depthEdge) return 1;
  const sample = sampleRgbGuidance(guidance, u, v);
  if (sample === null) return 0.45;
  if (sample.edgeStrength >= 0.36) return 0.08;
  if (sample.edgeStrength >= 0.20) return 0.22;
  return 0.42;
}
