import { afterEach, describe, expect, it, vi } from 'vitest';

import {
  DepthAnythingV2WebGpuProvider,
  type TransformersDepthPipeline,
  type TransformersModuleLike,
} from '../../src/xr/monocularDepthPrior';
import { MonocularPriorRuntime } from '../../src/xr/monocularPriorRuntime';

function deferred<T>() {
  let resolve!: (value: T) => void;
  let reject!: (reason?: unknown) => void;
  const promise = new Promise<T>((resolvePromise, rejectPromise) => {
    resolve = resolvePromise;
    reject = rejectPromise;
  });
  return { promise, resolve, reject };
}

function output() {
  return { predicted_depth: { data: new Float32Array([1.25, 1.5]), dims: [1, 2] } };
}

class FakeRawImage {
  constructor(readonly data: Uint8Array | Uint8ClampedArray) {}
}

function provider(loader: () => Promise<TransformersModuleLike>): DepthAnythingV2WebGpuProvider {
  vi.stubGlobal('navigator', { gpu: {} });
  vi.stubGlobal('document', {});
  return new DepthAnythingV2WebGpuProvider('fake-depth', 'fake://transformers', loader);
}

function moduleWith(pipeline: TransformersDepthPipeline): TransformersModuleLike {
  return {
    RawImage: FakeRawImage,
    pipeline: async () => pipeline,
  };
}

function fakePipeline(infer: () => Promise<unknown> = async () => output()) {
  const callable = Object.assign(vi.fn(infer), { dispose: vi.fn(async () => undefined) });
  return callable as unknown as TransformersDepthPipeline;
}

afterEach(() => vi.unstubAllGlobals());

describe('Depth Anything provider memory lifecycle', () => {
  it('awaits one real pipeline disposal after loading and inference', async () => {
    const pipeline = fakePipeline();
    const target = provider(async () => moduleWith(pipeline));
    const frame = await target.estimateRgba(new Uint8Array(8), 2, 1, 12);

    expect(frame).toMatchObject({ width: 2, height: 1, capturedAtMs: 12 });
    expect(await target.releaseMemory()).toBeUndefined();
    await target.dispose();
    expect(pipeline.dispose).toHaveBeenCalledTimes(1);
    expect(target.status).toBe('disabled');
  });

  it('does not create a pipeline when release happens during dynamic module loading', async () => {
    const imported = deferred<TransformersModuleLike>();
    const pipeline = fakePipeline();
    const factory = vi.fn(async () => pipeline);
    const target = provider(() => imported.promise);
    const request = target.estimateRgba(new Uint8Array(4), 1, 1, 1);

    const release = target.releaseMemory();
    imported.resolve({ RawImage: FakeRawImage, pipeline: factory });
    expect(await request).toBeNull();
    await release;
    expect(factory).not.toHaveBeenCalled();
    expect(pipeline.dispose).not.toHaveBeenCalled();
    expect(target.status).toBe('disabled');
  });

  it('disposes a pipeline that resolves after its loading generation is released', async () => {
    const created = deferred<TransformersDepthPipeline>();
    const pipelineStarted = deferred<void>();
    const pipeline = fakePipeline();
    const target = provider(async () => ({
      RawImage: FakeRawImage,
      pipeline: async () => {
        pipelineStarted.resolve();
        return created.promise;
      },
    }));
    const request = target.estimateRgba(new Uint8Array(4), 1, 1, 1);
    await pipelineStarted.promise;
    const release = target.releaseMemory();
    created.resolve(pipeline);

    expect(await request).toBeNull();
    await release;
    expect(pipeline.dispose).toHaveBeenCalledTimes(1);
    expect(target.status).toBe('disabled');
  });

  it('creates a fresh usable provider generation after disable and re-enable', async () => {
    const first = fakePipeline();
    const second = fakePipeline(async () => output());
    let loads = 0;
    const target = provider(async () => moduleWith(loads++ === 0 ? first : second));
    const runtime = new MonocularPriorRuntime(target);

    runtime.setEnabled(true);
    expect(await target.estimateRgba(new Uint8Array(4), 1, 1, 1)).not.toBeNull();
    runtime.setEnabled(false);
    await runtime.releaseMemory();
    runtime.setEnabled(true);
    expect(await target.estimateRgba(new Uint8Array(4), 1, 1, 2)).not.toBeNull();

    expect(loads).toBe(2);
    expect(first.dispose).toHaveBeenCalledTimes(1);
    expect(second.dispose).not.toHaveBeenCalled();
  });

  it('cannot let a pending disabled generation overwrite the re-enabled provider', async () => {
    const created = deferred<TransformersDepthPipeline>();
    const firstPipelineStarted = deferred<void>();
    const obsolete = fakePipeline();
    const fresh = fakePipeline();
    let loads = 0;
    const target = provider(async () => ({
      RawImage: FakeRawImage,
      pipeline: async () => {
        loads += 1;
        if (loads === 1) {
          firstPipelineStarted.resolve();
          return created.promise;
        }
        return fresh;
      },
    }));
    const runtime = new MonocularPriorRuntime(target);

    runtime.setEnabled(true);
    const oldRequest = target.estimateRgba(new Uint8Array(4), 1, 1, 1);
    await firstPipelineStarted.promise;
    runtime.setEnabled(false);
    const release = runtime.releaseMemory();
    runtime.setEnabled(true);
    const newRequest = target.estimateRgba(new Uint8Array(4), 1, 1, 2);
    created.resolve(obsolete);

    expect(await oldRequest).toBeNull();
    await release;
    expect(await newRequest).not.toBeNull();
    expect(obsolete.dispose).toHaveBeenCalledTimes(1);
    expect(fresh.dispose).not.toHaveBeenCalled();
    expect(target.status).toBe('ready');
  });

  it('ignores inference that resolves after release and disposes its pipeline', async () => {
    const inference = deferred<unknown>();
    const started = deferred<void>();
    const pipeline = fakePipeline(() => {
      started.resolve();
      return inference.promise;
    });
    const target = provider(async () => moduleWith(pipeline));
    const request = target.estimateRgba(new Uint8Array(4), 1, 1, 1);
    await started.promise;
    await target.releaseMemory();
    inference.resolve(output());

    expect(await request).toBeNull();
    expect(target.status).toBe('disabled');
    expect(pipeline.dispose).toHaveBeenCalledTimes(1);
  });

  it('returns live pipeline count to baseline through repeated release cycles', async () => {
    let livePipelines = 0;
    const target = provider(async () => {
      let disposed = false;
      const callable = Object.assign(vi.fn(async () => output()), {
        dispose: vi.fn(async () => {
          if (!disposed) {
            disposed = true;
            livePipelines -= 1;
          }
        }),
      });
      livePipelines += 1;
      return moduleWith(callable as unknown as TransformersDepthPipeline);
    });

    for (let cycle = 0; cycle < 3; cycle += 1) {
      expect(await target.estimateRgba(new Uint8Array(4), 1, 1, cycle)).not.toBeNull();
      await target.releaseMemory();
      expect(livePipelines).toBe(0);
    }
  });
});
