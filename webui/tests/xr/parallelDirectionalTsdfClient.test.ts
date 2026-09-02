import { describe, expect, it } from 'vitest';

import { recommendedParallelTsdfChannelWorkers } from '../../src/xr/parallelDirectionalTsdfClient';

describe('parallel directional TSDF worker budget', () => {
  it('keeps single-channel work on one worker', () => {
    expect(recommendedParallelTsdfChannelWorkers(6, 1)).toBe(1);
  });

  it('does not parallelize when the post-scan budget has no spare CPU worker', () => {
    expect(recommendedParallelTsdfChannelWorkers(1, 6)).toBe(1);
    expect(recommendedParallelTsdfChannelWorkers(0, 6)).toBe(1);
    expect(recommendedParallelTsdfChannelWorkers(Number.NaN, 6)).toBe(1);
  });

  it('uses available independent channels without exceeding the CPU budget', () => {
    expect(recommendedParallelTsdfChannelWorkers(2, 6)).toBe(2);
    expect(recommendedParallelTsdfChannelWorkers(6, 2)).toBe(2);
  });

  it('caps memory-heavy TSDF concurrency at three workers on large phones', () => {
    expect(recommendedParallelTsdfChannelWorkers(6, 6)).toBe(3);
    expect(recommendedParallelTsdfChannelWorkers(12, 6)).toBe(3);
    expect(recommendedParallelTsdfChannelWorkers(16, 6)).toBe(3);
  });
});
