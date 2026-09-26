import { describe, expect, it } from 'vitest';
import { AR } from '../../src/xr/fiducial/jsAruco2';

function markerImage(id: number): { width: number; height: number; rgba: Uint8Array } {
  const width = 240;
  const height = 240;
  const rgba = new Uint8Array(width * height * 4);
  rgba.fill(255);
  for (let i = 3; i < rgba.length; i += 4) rgba[i] = 255;
  const cell = 20;
  const x0 = 40;
  const y0 = 40;
  const bits = new AR.Dictionary('APRILTAG_36h11')._hex2bin(
    AR.DICTIONARIES.APRILTAG_36h11.codeList[id], 36,
  );
  for (let y = 0; y < 8; y += 1) for (let x = 0; x < 8; x += 1) {
    const white = x > 0 && x < 7 && y > 0 && y < 7 && bits[(y - 1) * 6 + x - 1] === '1';
    if (!white) for (let py = y0 + y * cell; py < y0 + (y + 1) * cell; py += 1) {
      for (let px = x0 + x * cell; px < x0 + (x + 1) * cell; px += 1) {
        const offset = (py * width + px) * 4;
        rgba[offset] = 0; rgba[offset + 1] = 0; rgba[offset + 2] = 0;
      }
    }
  }
  return { width, height, rgba };
}

describe('js-aruco2 CommonJS worker wrapper', () => {
  it('loads the AprilTag 36h11 dictionary in Vitest Node and decodes a registered code', () => {
    expect(AR.DICTIONARIES.APRILTAG_36h11).toMatchObject({ nBits: 36, tau: 11 });
    expect(AR.DICTIONARIES.APRILTAG_36h11.codeList.length).toBeGreaterThan(580);
    const image = markerImage(0);
    const detector = new AR.Detector({ dictionaryName: 'APRILTAG_36h11', maxHammingDistance: 2 });
    const markers = detector.detectImage(image.width, image.height, image.rgba);
    expect(markers.map((marker) => marker.id)).toContain(0);
  });
});
