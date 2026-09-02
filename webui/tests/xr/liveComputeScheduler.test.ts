import { describe, expect, it } from 'vitest';

import { LiveComputeScheduler } from '../../src/xr/liveComputeScheduler';

describe('live XR compute scheduler', () => {
  it('enforces one shared heavy-job slot across structural and visual clients', async () => {
    const scheduler = new LiveComputeScheduler(async () => 1);
    const structural = await scheduler.tryAcquire('structural');
    expect(structural).not.toBeNull();
    expect(await scheduler.tryAcquire('visual')).toBeNull();
    expect(scheduler.snapshot()).toEqual({ budget: 1, active: 1, structural: 1, visual: 0 });

    structural!.release();
    const visual = await scheduler.tryAcquire('visual');
    expect(visual).not.toBeNull();
    expect(scheduler.snapshot()).toEqual({ budget: 1, active: 1, structural: 0, visual: 1 });
    visual!.release();
  });

  it('allows two independent heavy jobs when the negotiated live budget is two', async () => {
    const scheduler = new LiveComputeScheduler(async () => 2);
    const structural = await scheduler.tryAcquire('structural');
    const visual = await scheduler.tryAcquire('visual');
    expect(structural).not.toBeNull();
    expect(visual).not.toBeNull();
    expect(await scheduler.tryAcquire('visual')).toBeNull();
    expect(scheduler.snapshot().active).toBe(2);
    structural!.release();
    visual!.release();
  });

  it('makes release idempotent so error cleanup cannot corrupt accounting', async () => {
    const scheduler = new LiveComputeScheduler(async () => 1);
    const lease = await scheduler.tryAcquire('visual');
    expect(lease).not.toBeNull();
    lease!.release();
    lease!.release();
    expect(scheduler.snapshot().active).toBe(0);
    expect((await scheduler.tryAcquire('structural'))).not.toBeNull();
  });

  it('refuses live heavy work when the capability profile negotiated zero slots', async () => {
    const scheduler = new LiveComputeScheduler(async () => 0);
    expect(await scheduler.tryAcquire('structural')).toBeNull();
    expect(scheduler.snapshot().budget).toBe(0);
  });

  it('fails closed instead of leaking a rejected capability probe into clients', async () => {
    const scheduler = new LiveComputeScheduler(async () => {
      throw new Error('probe failed');
    });
    expect(await scheduler.tryAcquire('visual')).toBeNull();
    expect(scheduler.snapshot()).toEqual({ budget: 0, active: 0, structural: 0, visual: 0 });
  });

  it('does not queue stale work behind a busy slot', async () => {
    const scheduler = new LiveComputeScheduler(async () => 1);
    const first = await scheduler.tryAcquire('visual');
    const second = scheduler.tryAcquire('structural');
    await expect(second).resolves.toBeNull();
    first!.release();
  });
});
