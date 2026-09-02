import { describe, expect, it } from 'vitest';

import { buildRgbDepthGuidanceFrameFromLuma } from '../../src/xr/rgbDepthGuidance';
import { VisualLineEvidenceClient } from '../../src/xr/visualLineEvidenceClient';

const identity = new Float32Array([
  1, 0, 0, 0,
  0, 1, 0, 0,
  0, 0, 1, 0,
  0, 0, 0, 1,
]);

function lineFrame() {
  const width = 64;
  const height = 48;
  const luma = new Uint8Array(width * height);
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) luma[y * width + x] = x < 31 ? 20 : 235;
  }
  return buildRgbDepthGuidanceFrameFromLuma(luma, width, height, 100);
}

describe('visual line evidence client', () => {
  it('keeps the non-worker fallback asynchronous and backpressured for offline callers', async () => {
    const client = new VisualLineEvidenceClient();
    try {
      if (client.workerBacked) return;
      const pending = client.analyze(lineFrame(), 3, identity, identity);
      expect(client.busy).toBe(true);
      const resolved = await pending;
      expect(resolved.keyframeId).toBe(3);
      expect(resolved.lines.length).toBeGreaterThan(0);
      expect(client.busy).toBe(false);
    } finally {
      client.dispose();
    }
  });

  it('rejects a queued fallback result after scan reset instead of leaking it into the next epoch', async () => {
    const client = new VisualLineEvidenceClient();
    try {
      if (client.workerBacked) return;
      const pending = client.analyze(lineFrame(), 7, identity, identity);
      expect(client.busy).toBe(true);
      client.reset();
      // The old event-loop task remains counted as work until it self-rejects;
      // a new scan therefore cannot stack another synchronous Hough fallback.
      expect(client.busy).toBe(true);
      await expect(pending).rejects.toThrow('obsolete scan epoch');
      expect(client.busy).toBe(false);
    } finally {
      client.dispose();
    }
  });

  it('never executes the heavy fallback for a live-XR configured client', async () => {
    const client = new VisualLineEvidenceClient({
      allowMainThreadFallback: false,
      deferWorkerUntilAnalyze: true,
    });
    try {
      if (client.workerBacked) return;
      await expect(client.analyze(lineFrame(), 9, identity, identity)).rejects.toThrow(/Worker/);
      expect(client.busy).toBe(false);
    } finally {
      client.dispose();
    }
  });
});
