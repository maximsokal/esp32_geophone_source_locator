import { describe, expect, it } from 'vitest';

import { isCurrentStructuralGuideWorkerResult } from '../../src/xr/structuralGuideProtocol';
import { LiveComputeScheduler } from '../../src/xr/liveComputeScheduler';
import { StructuralGuideClient } from '../../src/xr/structuralGuideClient';

class HoldingStructuralWorker {
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

describe('structural guide worker epoch guard', () => {
  it('parks and replaces the structural worker across the live XR -> post-scan boundary', async () => {
    const workers = [new HoldingStructuralWorker(), new HoldingStructuralWorker()];
    let workerIndex = 0;
    const client = new StructuralGuideClient(
      () => workers[workerIndex++] as unknown as Worker,
      new LiveComputeScheduler(async () => 1),
    );
    const input = {
      positions: new Float32Array([0, 0, 0]),
      normals: new Float32Array([0, 0, 1]),
      confidence: new Float32Array([1]),
      keyframeCount: 1,
      voxelSizeMeters: 0.04,
    } as Parameters<typeof client.analyze>[0];

    const liveJob = client.analyze(input);
    await workers[0].analyzePosted;
    client.park();
    await expect(liveJob).rejects.toThrow('parked after XR ended');
    expect(workers[0].terminated).toBe(true);

    const nextSessionJob = client.analyze(input);
    await workers[1].analyzePosted;
    client.park();
    await expect(nextSessionJob).rejects.toThrow('parked after XR ended');
    expect(workers[1].terminated).toBe(true);
    client.dispose();
  });

  it('accepts only the matching request from the active scan epoch', () => {
    expect(isCurrentStructuralGuideWorkerResult(
      { requestId: 7, epoch: 3 },
      { id: 7, epoch: 3 },
      3,
    )).toBe(true);
  });

  it('rejects a result from the previous scan even when request ids look plausible', () => {
    expect(isCurrentStructuralGuideWorkerResult(
      { requestId: 7, epoch: 2 },
      { id: 7, epoch: 2 },
      3,
    )).toBe(false);
  });

  it('rejects an old request after a new analysis has replaced the pending slot', () => {
    expect(isCurrentStructuralGuideWorkerResult(
      { requestId: 7, epoch: 3 },
      { id: 8, epoch: 3 },
      3,
    )).toBe(false);
  });

  it('rejects results when there is no pending analysis', () => {
    expect(isCurrentStructuralGuideWorkerResult({ requestId: 7, epoch: 3 }, null, 3)).toBe(false);
  });
});
