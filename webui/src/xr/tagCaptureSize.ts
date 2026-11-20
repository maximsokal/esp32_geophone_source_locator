/**
 * Size of the AprilTag capture target. The phone's raw camera image is not 16:9: phone run 2026-11-20_165835 reported a
 * portrait 886 x 1920 image with the same aspect as the view's projection. Drawing it into a fixed landscape 1280 x 720
 * target stretched every tag about 3.9x wider than tall (a 32 mm tag at 0.4 m: 160 x 42 px instead of ~83 x 83), which
 * wasted most of the vertical resolution, cut the reach and made corners coarse. The target now follows the camera aspect and
 * spends the same pixel budget, so the cost of a capture (blit, readback, decode) does not grow.
 */
export const TAG_CAPTURE_PIXEL_BUDGET = 1280 * 720;
const MIN_SIDE = 160;
const MAX_SIDE = 2048;

export interface TagCaptureSize { width: number; height: number; }

const even = (value: number): number => 2 * Math.round(value / 2);

/** Same-aspect size with about `budget` pixels, both sides even. Falls back to 1280 x 720 for an unusable camera size. */
export function tagCaptureSizeFor(cameraWidth: number, cameraHeight: number, budget: number = TAG_CAPTURE_PIXEL_BUDGET): TagCaptureSize {
  if (!Number.isFinite(cameraWidth) || !Number.isFinite(cameraHeight) || cameraWidth <= 0 || cameraHeight <= 0) return { width: 1280, height: 720 };
  const scale = Math.sqrt(budget / (cameraWidth * cameraHeight));
  const clamp = (side: number): number => Math.max(MIN_SIDE, Math.min(MAX_SIDE, side));
  return { width: clamp(even(cameraWidth * scale)), height: clamp(even(cameraHeight * scale)) };
}
