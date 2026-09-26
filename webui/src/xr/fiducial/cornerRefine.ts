export type PixelPoint = readonly [x: number, y: number];
export type PixelQuad = readonly [PixelPoint, PixelPoint, PixelPoint, PixelPoint];

interface Line { nx: number; ny: number; c: number; }

function grayscale(rgba: Uint8Array, width: number, height: number): Float32Array {
  const output = new Float32Array(width * height);
  for (let pixel = 0; pixel < output.length; pixel += 1) {
    const offset = pixel * 4;
    output[pixel] = rgba[offset] * 0.299 + rgba[offset + 1] * 0.587 + rgba[offset + 2] * 0.114;
  }
  return output;
}

function lumaAt(luma: Float32Array, width: number, height: number, x: number, y: number): number {
  const px = Math.max(0, Math.min(width - 1.001, x - 0.5));
  const py = Math.max(0, Math.min(height - 1.001, y - 0.5));
  const x0 = Math.floor(px); const y0 = Math.floor(py); const x1 = x0 + 1; const y1 = y0 + 1;
  const tx = px - x0; const ty = py - y0;
  const a = luma[y0 * width + x0] * (1 - tx) + luma[y0 * width + x1] * tx;
  const b = luma[y1 * width + x0] * (1 - tx) + luma[y1 * width + x1] * tx;
  return a * (1 - ty) + b * ty;
}

function edgeLine(a: PixelPoint, b: PixelPoint, luma: Float32Array, width: number, height: number): Line | null {
  const dx = b[0] - a[0]; const dy = b[1] - a[1]; const length = Math.hypot(dx, dy);
  if (length < 4) return null;
  const nx = -dy / length; const ny = dx / length;
  const samples: { x: number; y: number; weight: number }[] = [];
  for (let along = 0.12; along <= 0.88; along += 1.5 / length) {
    const x = a[0] + dx * along; const y = a[1] + dy * along;
    let nearestOffset = Number.POSITIVE_INFINITY;
    let selectedX = x; let selectedY = y; let strength = 0;
    let previousOffset = -2.5;
    let previous = lumaAt(luma, width, height, x + nx * previousOffset, y + ny * previousOffset);
    for (let offset = previousOffset + 0.25; offset <= 2.5; offset += 0.25) {
      const current = lumaAt(luma, width, height, x + nx * offset, y + ny * offset);
      if ((previous - 127.5) * (current - 127.5) <= 0 && previous !== current) {
        const fraction = Math.max(0, Math.min(1, (127.5 - previous) / (current - previous)));
        const crossing = previousOffset + (offset - previousOffset) * fraction;
        const gradient = Math.abs(current - previous) / (offset - previousOffset);
        if (Math.abs(crossing) < nearestOffset) {
          nearestOffset = Math.abs(crossing);
          selectedX = x + nx * crossing;
          selectedY = y + ny * crossing;
          strength = gradient;
        }
      }
      previousOffset = offset;
      previous = current;
    }
    if (strength > 0) samples.push({ x: selectedX, y: selectedY, weight: strength });
  }
  if (samples.length < 4) return null;

  // Gradient magnitude supplies the weights for the total least-squares line fit.
  const weight = samples.reduce((sum, sample) => sum + sample.weight, 0);
  const mx = samples.reduce((sum, sample) => sum + sample.x * sample.weight, 0) / weight;
  const my = samples.reduce((sum, sample) => sum + sample.y * sample.weight, 0) / weight;
  let xx = 0; let xy = 0; let yy = 0;
  for (const sample of samples) {
    const x = sample.x - mx; const y = sample.y - my;
    xx += sample.weight * x * x; xy += sample.weight * x * y; yy += sample.weight * y * y;
  }
  const angle = 0.5 * Math.atan2(2 * xy, xx - yy);
  const tx = Math.cos(angle); const ty = Math.sin(angle);
  const normalX = -ty; const normalY = tx;
  return { nx: normalX, ny: normalY, c: -(normalX * mx + normalY * my) };
}

function intersect(a: Line, b: Line): [number, number] | null {
  const determinant = a.nx * b.ny - a.ny * b.nx;
  if (Math.abs(determinant) < 1e-5) return null;
  return [(-a.c * b.ny + a.ny * b.c) / determinant, (-a.nx * b.c + a.c * b.nx) / determinant];
}

/**
 * Subpixel refinement: sample each of the four tag edges across its luma
 * transition, fit each line with gradient-weighted total least squares, then
 * intersect adjacent lines to recover the four corners.
 */
export function refineTagCorners(corners: PixelQuad, rgba: Uint8Array, width: number, height: number): PixelQuad {
  if (width < 3 || height < 3 || rgba.length < width * height * 4) return corners;
  const luma = grayscale(rgba, width, height);
  const lines = corners.map((corner, index) => edgeLine(corner, corners[(index + 1) % 4], luma, width, height));
  if (lines.some((line) => line === null)) return corners;
  const fitted = lines as Line[];
  const result = corners.map((_, index) => intersect(fitted[(index + 3) % 4], fitted[index]));
  return result.some((point) => point === null)
    ? corners
    : result as unknown as PixelQuad;
}
