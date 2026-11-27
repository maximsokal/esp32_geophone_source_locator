import { StrictMode, createElement } from 'react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { act, create, type ReactTestRenderer } from 'react-test-renderer';

import { LiveComputeScheduler } from '../../src/xr/liveComputeScheduler';
import { StructuralGuideClient } from '../../src/xr/structuralGuideClient';
import { useStructuralGuideClient } from '../../src/xr/useStructuralGuideClient';

class FakeWorker {
  terminated = false;
  posted: Array<Record<string, unknown>> = [];
  private readonly messageListeners = new Set<(event: MessageEvent) => void>();
  addEventListener(type: string, listener: EventListenerOrEventListenerObject): void {
    if (type === 'message' && typeof listener === 'function') this.messageListeners.add(listener as (event: MessageEvent) => void);
  }
  removeEventListener(type: string, listener: EventListenerOrEventListenerObject): void {
    if (type === 'message' && typeof listener === 'function') this.messageListeners.delete(listener as (event: MessageEvent) => void);
  }
  postMessage(message: Record<string, unknown>): void { this.posted.push(message); }
  terminate(): void { this.terminated = true; }
  /** A worker answers the analysis it was given (listeners are detached once the client terminated it). */
  answer(): void {
    const request = this.posted.filter((message) => message.type === 'analyze').at(-1);
    if (request === undefined) throw new Error('no analysis was posted');
    const result = { edges: [], corners: [], computeMs: 1 };
    for (const listener of [...this.messageListeners]) listener({ data: { type: 'result', requestId: request.requestId, epoch: request.epoch, result } } as MessageEvent);
  }
}

const input = () => ({
  positions: new Float32Array([0, 0, 0]), normals: new Float32Array([0, 0, 1]), confidence: new Float32Array([1]), keyframeCount: 1, voxelSizeMeters: 0.04, planePriors: [],
}) as Parameters<StructuralGuideClient['analyze']>[0];

const harness = () => {
  const workers: FakeWorker[] = [];
  const client = new StructuralGuideClient(() => { const worker = new FakeWorker(); workers.push(worker); return worker as unknown as Worker; }, new LiveComputeScheduler(async () => 1));
  return { workers, client };
};
/** Waits until worker `index` has been handed `count` analyses. */
const posted = async (workers: FakeWorker[], index: number, count = 1): Promise<FakeWorker> => {
  for (let i = 0; i < 100 && (workers[index]?.posted.filter((message) => message.type === 'analyze').length ?? 0) < count; i += 1) await Promise.resolve();
  return workers[index];
};

describe('structural guide client lifecycle (TASK 06: WORKER-01..07)', () => {
  it('WORKER-01 start -> finish -> start: the second analysis works on the same client', async () => {
    const { workers, client } = harness();
    const first = client.analyze(input());
    (await posted(workers, 0)).answer();
    await expect(first).resolves.toMatchObject({ computeMs: 1 });
    const second = client.analyze(input());
    (await posted(workers, 0, 2)).answer();
    await expect(second).resolves.toBeDefined();
    expect(workers).toHaveLength(1);                                  /* the worker is reused, not recreated */
  });

  it('WORKER-02 start -> cancel (reset for a new scan) -> start: the cancelled one rejects as a reset, the next one runs on a fresh worker', async () => {
    const { workers, client } = harness();
    const cancelled = client.analyze(input());
    await posted(workers, 0);
    client.reset();
    await expect(cancelled).rejects.toThrow('reset for a new scan');
    expect(workers[0].terminated).toBe(true);
    const next = client.analyze(input());
    (await posted(workers, 1)).answer();
    await expect(next).resolves.toBeDefined();
  });

  it('WORKER-03 dispose during an active computation rejects it once and terminates the worker', async () => {
    const { workers, client } = harness();
    const running = client.analyze(input());
    await posted(workers, 0);
    client.dispose();
    await expect(running).rejects.toThrow('Structural guide worker disposed.');
    expect(workers[0].terminated).toBe(true);
  });

  it('WORKER-04 a late answer of a destroyed worker is ignored: nothing resolves, nothing throws', async () => {
    const { workers, client } = harness();
    const running = client.analyze(input());
    const worker = await posted(workers, 0);
    client.park();
    await expect(running).rejects.toThrow('parked after XR ended');
    expect(() => worker.answer()).not.toThrow();
  });

  it('WORKER-05/07 ten sequential XR sessions leave no accumulated worker: every parked worker is terminated, at most one is alive', async () => {
    const { workers, client } = harness();
    for (let session = 0; session < 10; session += 1) {
      client.reset();                                                 /* XR starting */
      const job = client.analyze(input());
      (await posted(workers, session)).answer();                  /* each session creates exactly one new worker */
      await job;
      client.park();                                                  /* XR ended */
    }
    expect(workers).toHaveLength(10);
    expect(workers.filter((worker) => !worker.terminated)).toHaveLength(0);
    client.dispose();
  });

  it('a disposed client refuses new work with the documented message (a real unmount stays final)', async () => {
    const { client } = harness();
    client.dispose();
    await expect(client.analyze(input())).rejects.toThrow('Structural guide worker disposed.');
  });
});

describe('the overlay\'s client under React StrictMode (the 40x "Structural guide worker disposed." of the 2026-11-26 run)', () => {
  const mounted: ReactTestRenderer[] = [];
  beforeEach(() => { vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true); });
  afterEach(() => { for (const renderer of mounted.splice(0)) act(() => renderer.unmount()); vi.unstubAllGlobals(); });

  /** `kept` receives the client the component holds on every render: the last entry is the one in use. */
  function Probe({ kept }: { kept: StructuralGuideClient[] }) {
    const client = useStructuralGuideClient(() => harness().client);
    kept.push(client);
    return null;
  }
  const mountStrict = (kept: StructuralGuideClient[]): ReactTestRenderer => {
    let renderer!: ReactTestRenderer;
    act(() => { renderer = create(createElement(StrictMode, null, createElement(Probe, { kept }))); });
    mounted.push(renderer);
    return renderer;
  };

  it('stays usable after StrictMode ran the cleanup and the setup again', async () => {
    const kept: StructuralGuideClient[] = [];
    mountStrict(kept);
    const client = kept[kept.length - 1];
    const job = client.analyze(input());
    const outcome = await Promise.race([job.then(() => 'resolved', (error: Error) => error.message), new Promise<string>((resolve) => setTimeout(() => resolve('pending'), 20))]);
    expect(outcome, 'it must not have been disposed by the simulated unmount').not.toBe('Structural guide worker disposed.');
  });

  it('is disposed by a REAL unmount', async () => {
    const kept: StructuralGuideClient[] = [];
    const renderer = mountStrict(kept);
    act(() => renderer.unmount());
    mounted.splice(mounted.indexOf(renderer), 1);
    await expect(kept[kept.length - 1].analyze(input())).rejects.toThrow('Structural guide worker disposed.');
  });
});
