import { refineTagCorners, type PixelQuad } from './cornerRefine';
import { AR, type ArucoMarker } from './jsAruco2';
import type { NormalizedTagCorner } from './tagPose';

// js-aruco2 compares distance < tau, so 2 permits at most one corrected bit;
// AprilTag 36h11 can correct more, but one bit is the conservative false-ID limit here.
export const TAG_DETECTOR_MAX_HAMMING_DISTANCE = 2;
export const TAG_DETECTOR_MIN_EDGE_PX = 12;

export interface DetectedAprilTag {
  tagId: 0 | 1 | 2 | 3 | 4;
  /** tagPose order: bottom-left, bottom-right, top-right, top-left; v increases down. */
  corners: readonly [NormalizedTagCorner, NormalizedTagCorner, NormalizedTagCorner, NormalizedTagCorner];
  hammingDistance: number;
}

export function mapArucoCornersToTagPose(corners: PixelQuad): PixelQuad {
  // js-aruco2 returns code-upright clockwise image corners (top-left first).
  // Reverse the cycle to tagPose's counter-clockwise bottom-left-first contract.
  return [corners[3], corners[2], corners[1], corners[0]];
}

function pixelCorners(marker: ArucoMarker): PixelQuad {
  return marker.corners.slice(0, 4).map((point) => [point.x, point.y] as const) as unknown as PixelQuad;
}

function edgeSize(corners: PixelQuad): number {
  return Math.min(...corners.map((point, index) => Math.hypot(
    point[0] - corners[(index + 1) % 4][0], point[1] - corners[(index + 1) % 4][1],
  )));
}

export function detectAprilTags(width: number, height: number, rgba: Uint8Array): DetectedAprilTag[] {
  if (width <= 0 || height <= 0 || rgba.length < width * height * 4) return [];
  const detector = new AR.Detector({ dictionaryName: 'APRILTAG_36h11', maxHammingDistance: TAG_DETECTOR_MAX_HAMMING_DISTANCE });
  const markers = detector.detectImage(width, height, rgba);
  const result: DetectedAprilTag[] = [];
  for (const marker of markers) {
    if (marker.id < 0 || marker.id > 4 || marker.corners.length !== 4) continue;
    const raw = pixelCorners(marker);
    if (edgeSize(raw) < TAG_DETECTOR_MIN_EDGE_PX) continue;
    const refined = refineTagCorners(raw, rgba, width, height);
    const ordered = mapArucoCornersToTagPose(refined);
    result.push({
      tagId: marker.id as 0 | 1 | 2 | 3 | 4,
      corners: ordered.map(([x, y]) => [x / (width - 1), y / (height - 1)] as const) as unknown as DetectedAprilTag['corners'],
      hammingDistance: marker.hammingDistance,
    });
  }
  return onePerTagId(result);
}

function quadArea(corners: DetectedAprilTag['corners']): number {
  let twice = 0;
  for (let i = 0; i < 4; i += 1) {
    const [x0, y0] = corners[i]; const [x1, y1] = corners[(i + 1) % 4];
    twice += x0 * y1 - x1 * y0;
  }
  return Math.abs(twice) / 2;
}

/**
 * Each tag id is one physical sticker, so a frame holds it at most once. The contour search can report the same sticker twice (the
 * outer border and a quad a couple of pixels inside it, seen at 1280x720 from a 15 degree view); keeping both double-counts one
 * observation with a slightly different pose. Keep the cleaner reading: fewest corrected bits, then the larger quad.
 */
function onePerTagId(detections: DetectedAprilTag[]): DetectedAprilTag[] {
  const best = new Map<number, DetectedAprilTag>();
  for (const detection of detections) {
    const held = best.get(detection.tagId);
    if (held === undefined || detection.hammingDistance < held.hammingDistance ||
        (detection.hammingDistance === held.hammingDistance && quadArea(detection.corners) > quadArea(held.corners))) {
      best.set(detection.tagId, detection);
    }
  }
  return detections.filter((detection) => best.get(detection.tagId) === detection);
}
