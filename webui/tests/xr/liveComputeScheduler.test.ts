import { describe, expect, it } from 'vitest';

import { LiveComputeScheduler, type LiveComputePressure } from '../../src/xr/liveComputeScheduler';

function snapshotCore(scheduler: LiveComputeScheduler) {
  const value = scheduler.snapshot();
  return {
    budget: value.budget,
    maximumBudget: value.maximumBudget,
    effectiveBudget: value.effectiveBudget,
    pressure: value.pressure,
    degraded: value.degraded,
    active: value.active,
    structural: value.structural,
    visual: value.visual,
  };
}

describe('live XR compute scheduler', () => {
  it('resolves the fixed compute ceiling during pre-scan preparation without acquiring work', async () => {
    let probes = 0;
    const scheduler = new LiveComputeScheduler(async () => {
      probes += 1;
      return 2;
    });

    expect(scheduler.snapshot().maximumBudget).toBeNull();
    const prepared = await scheduler.prepare();
    expect(prepared.maximumBudget).toBe(2);
    expect(prepared.effectiveBudget).toBe(2);
    expect(prepared.active).toBe(0);
    expect(probes).toBe(1);

    await scheduler.prepare();
    const lease = await scheduler.tryAcquire('structural');
    expect(lease).not.toBeNull();
    expect(probes).toBe(1);
    lease!.release();
  });

  it('reserves a single negotiated live slot for structural geometry', async () => {
    const scheduler = new LiveComputeScheduler(async () => 1);
    const structural = await scheduler.tryAcquire('structural');
    expect(structural).not.toBeNull();
    expect(await scheduler.tryAcquire('visual')).toBeNull();
    expect(snapshotCore(scheduler)).toEqual({
      budget: 1,
      maximumBudget: 1,
      effectiveBudget: 1,
      pressure: 'low',
      degraded: false,
      active: 1,
      structural: 1,
      visual: 0,
    });

    structural!.release();
    expect(await scheduler.tryAcquire('visual')).toBeNull();
    expect((await scheduler.tryAcquire('structural'))).not.toBeNull();
  });

  it('allows visual work only in the spare slot of a W2 device', async () => {
    const scheduler = new LiveComputeScheduler(async () => 2);
    const visual = await scheduler.tryAcquire('visual');
    expect(visual).not.toBeNull();
    expect(await scheduler.tryAcquire('visual')).toBeNull();

    const structural = await scheduler.tryAcquire('structural');
    expect(structural).not.toBeNull();
    expect(snapshotCore(scheduler).active).toBe(2);
    expect(snapshotCore(scheduler).structural).toBe(1);
    expect(snapshotCore(scheduler).visual).toBe(1);
    structural!.release();
    visual!.release();
  });

  it('makes release idempotent so error cleanup cannot corrupt accounting', async () => {
    const scheduler = new LiveComputeScheduler(async () => 2);
    const lease = await scheduler.tryAcquire('visual');
    expect(lease).not.toBeNull();
    lease!.release();
    lease!.release();
    expect(scheduler.snapshot().active).toBe(0);
    expect((await scheduler.tryAcquire('structural'))).not.toBeNull();
  });

  it('drops from the hardware maximum to W1 immediately on high XR pressure', async () => {
    let pressure: LiveComputePressure = 'low';
    let now = 0;
    const scheduler = new LiveComputeScheduler(async () => 2, () => pressure, () => now);
    const warm = await scheduler.tryAcquire('structural');
    warm!.release();
    expect(scheduler.snapshot().effectiveBudget).toBe(2);

    pressure = 'high';
    now = 100;
    expect(await scheduler.tryAcquire('visual')).toBeNull();
    expect(snapshotCore(scheduler)).toEqual({
      budget: 1,
      maximumBudget: 2,
      effectiveBudget: 1,
      pressure: 'high',
      degraded: true,
      active: 0,
      structural: 0,
      visual: 0,
    });
    const structural = await scheduler.tryAcquire('structural');
    expect(structural).not.toBeNull();
    structural!.release();
  });

  it('requires sustained medium pressure before reducing W2 to W1', async () => {
    let now = 0;
    const scheduler = new LiveComputeScheduler(async () => 2, () => 'low', () => now);
    const warm = await scheduler.tryAcquire('structural');
    warm!.release();

    scheduler.observeRuntimePressure('medium', 800);
    expect(scheduler.snapshot().effectiveBudget).toBe(2);
    scheduler.observeRuntimePressure('medium', 1600);
    expect(scheduler.snapshot().effectiveBudget).toBe(2);
    scheduler.observeRuntimePressure('medium', 2400);
    expect(scheduler.snapshot().effectiveBudget).toBe(1);
    expect(scheduler.snapshot().degraded).toBe(true);
  });

  it('recovers from W1 only after five independent low-pressure windows', async () => {
    let now = 0;
    const scheduler = new LiveComputeScheduler(async () => 2, () => 'low', () => now);
    const warm = await scheduler.tryAcquire('structural');
    warm!.release();
    scheduler.observeRuntimePressure('high', 100);
    expect(scheduler.snapshot().effectiveBudget).toBe(1);

    for (const time of [900, 1700, 2500, 3300]) {
      scheduler.observeRuntimePressure('low', time);
      expect(scheduler.snapshot().effectiveBudget).toBe(1);
    }
    scheduler.observeRuntimePressure('low', 4100);
    expect(scheduler.snapshot().effectiveBudget).toBe(2);
    expect(scheduler.snapshot().degraded).toBe(false);
  });

  it('preserves a hot-device W1 cap across a scan restart', async () => {
    const scheduler = new LiveComputeScheduler(async () => 2, () => 'low', () => 0);
    const warm = await scheduler.tryAcquire('structural');
    warm!.release();
    scheduler.observeRuntimePressure('high', 100);
    expect(scheduler.snapshot().effectiveBudget).toBe(1);

    scheduler.beginScan();
    expect(scheduler.snapshot().effectiveBudget).toBe(1);
    expect(scheduler.snapshot().degraded).toBe(true);
    expect(await scheduler.tryAcquire('visual')).toBeNull();
  });

  it('resets recovery streaks at a scan boundary instead of inheriting four healthy samples', async () => {
    const scheduler = new LiveComputeScheduler(async () => 2, () => 'low', () => 0);
    const warm = await scheduler.tryAcquire('structural');
    warm!.release();
    scheduler.observeRuntimePressure('high', 100);
    for (const time of [900, 1700, 2500, 3300]) scheduler.observeRuntimePressure('low', time);
    expect(scheduler.snapshot().effectiveBudget).toBe(1);

    scheduler.beginScan();
    // A new session must earn five fresh low-pressure windows; one healthy sample
    // cannot complete the previous scan's nearly finished recovery streak.
    scheduler.observeRuntimePressure('low', 4100);
    expect(scheduler.snapshot().effectiveBudget).toBe(1);
  });

  it('does not count rapid UI snapshots as independent hysteresis windows', async () => {
    const scheduler = new LiveComputeScheduler(async () => 2, () => 'low', () => 0);
    const warm = await scheduler.tryAcquire('structural');
    warm!.release();
    scheduler.observeRuntimePressure('high', 100);
    for (const time of [200, 300, 400, 500, 600, 700]) scheduler.observeRuntimePressure('low', time);
    expect(scheduler.snapshot().effectiveBudget).toBe(1);
  });

  it('refuses live heavy work when the capability profile negotiated zero slots', async () => {
    const scheduler = new LiveComputeScheduler(async () => 0);
    expect(await scheduler.tryAcquire('structural')).toBeNull();
    expect(scheduler.snapshot().budget).toBe(0);
    expect(scheduler.snapshot().maximumBudget).toBe(0);
  });

  it('fails closed instead of leaking a rejected capability probe into clients', async () => {
    const scheduler = new LiveComputeScheduler(async () => {
      throw new Error('probe failed');
    });
    expect(await scheduler.tryAcquire('visual')).toBeNull();
    expect(snapshotCore(scheduler)).toEqual({
      budget: 0,
      maximumBudget: 0,
      effectiveBudget: 0,
      pressure: 'low',
      degraded: false,
      active: 0,
      structural: 0,
      visual: 0,
    });
  });

  it('does not queue stale work behind a busy effective slot', async () => {
    const scheduler = new LiveComputeScheduler(async () => 2);
    const visual = await scheduler.tryAcquire('visual');
    const structural = await scheduler.tryAcquire('structural');
    expect(visual).not.toBeNull();
    expect(structural).not.toBeNull();
    await expect(scheduler.tryAcquire('structural')).resolves.toBeNull();
    structural!.release();
    visual!.release();
  });
});
