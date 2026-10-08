import { afterEach, describe, expect, it, vi } from 'vitest';

import { LiveComputeScheduler } from '../../src/xr/liveComputeScheduler';
import { buildRgbDepthGuidanceFrameFromLuma } from '../../src/xr/rgbDepthGuidance';
import { VisualLineEvidenceClient } from '../../src/xr/visualLineEvidenceClient';

class HoldingVisualWorker {
  terminated = false;
  private readonly messageListeners = new Set<(event: MessageEvent) => void>();
  private readonly errorListeners = new Set<(event: ErrorEvent) => void>();
  private resolveAnalyzePosted!: () => void;
  readonly analyzePosted = new Promise<void>((resolve) => { this.resolveAnalyzePosted = resolve; });

  addEventListener(type: string, listener: EventListenerOrEventListenerObject): void {
    if (typeof listener !== 'function') return;
    if (type === 'message') this.messageListeners.add(listener as (event: MessageEvent) => void);
    if (type === 'error') this.errorListeners.add(listener as (event: ErrorEvent) => void);
  }

  removeEventListener(type: string, listener: EventListenerOrEventListenerObject): void {
    if (typeof listener !== 'function') return;
    if (type === 'message') this.messageListeners.delete(listener as (event: MessageEvent) => void);
    if (type === 'error') this.errorListeners.delete(listener as (event: ErrorEvent) => void);
  }

  postMessage(message: Record<string, unknown>): void {
    if (message.type === 'analyze') this.resolveAnalyzePosted();
  }

  terminate(): void { this.terminated = true; }
}

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
  afterEach(() => vi.unstubAllGlobals());

  it('terminates the visual-evidence role at Finish and uses a fresh worker for the next scan', async () => {
    vi.stubGlobal('Worker', class {});
    const workers = [new HoldingVisualWorker(), new HoldingVisualWorker()];
    let workerIndex = 0;
    const makeClient = () => new VisualLineEvidenceClient(
      { allowMainThreadFallback: false, deferWorkerUntilAnalyze: true },
      () => workers[workerIndex++] as unknown as Worker,
      new LiveComputeScheduler(async () => 2),
    );
    const first = makeClient();
    const firstJob = first.analyze(lineFrame(), 1, identity, identity);
    await workers[0].analyzePosted;
    first.dispose();
    await expect(firstJob).rejects.toThrow('disposed');
    expect(workers[0].terminated).toBe(true);

    const second = makeClient();
    const secondJob = second.analyze(lineFrame(), 2, identity, identity);
    await workers[1].analyzePosted;
    second.dispose();
    await expect(secondJob).rejects.toThrow('disposed');
    expect(workers[1].terminated).toBe(true);
    expect(workers[1]).not.toBe(workers[0]);
  });

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
      await expect(client.analyze(lineFrame(), 9, identity, identity)).rejects.toThrow(/Worker|compute budget/);
      expect(client.busy).toBe(false);
    } finally {
      client.dispose();
    }
  });
});
