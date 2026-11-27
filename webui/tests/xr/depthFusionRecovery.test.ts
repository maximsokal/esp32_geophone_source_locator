import { describe, expect, it, vi } from 'vitest';

import { captureFusionState, restoreFusionState } from '../../src/xr/depthFusionCheckpoint';
import type { FusionRecoveryReport } from '../../src/xr/depthFusionClient';
import { DepthFusionCore } from '../../src/xr/depthFusionCore';
import { RuntimeFusion, type RuntimeFusionOptions } from '../../src/xr/depthFusionRuntime';
import type { ScanPointCloudSnapshot } from '../../src/xr/depthPointCloudV2';
import { frameContext, HandlerWorker, KeeperWorker, orbitFrames, settle, snapshotDigest, type OrbitFrame } from './support/fusionHarness';

/*
 * Crash recovery of the fusion worker (Phase 2, stage 1). Acceptance: a scan whose worker dies at 10 %, 50 % or 90 %, or during Finish, ends with a snapshot
 * BIT-IDENTICAL to an uninterrupted scan of the same frames; whatever cannot be recovered is counted and reported.
 */
const FRAMES = 60;
const frames: OrbitFrame[] = orbitFrames(FRAMES);

interface Rig {
  fusion: RuntimeFusion;
  workers: HandlerWorker[];
  keepers: KeeperWorker[];
  recoveries: FusionRecoveryReport[];
  failures: string[];
  feed: (from: number, to: number) => Promise<void>;
}

function rig(options: RuntimeFusionOptions & { onWorker?: (worker: HandlerWorker, index: number) => void; noKeeper?: boolean } = {}): Rig {
  const workers: HandlerWorker[] = []; const keepers: KeeperWorker[] = []; const recoveries: FusionRecoveryReport[] = []; const failures: string[] = [];
  const { onWorker, noKeeper, ...rest } = options;
  const fusion = new RuntimeFusion({
    checkpointEveryFrames: 8,
    ...rest,
    workerFactory: () => { const worker = new HandlerWorker(); onWorker?.(worker, workers.length); workers.push(worker); return worker as unknown as Worker; },
    keeperFactory: noKeeper ? () => null : () => { const keeper = new KeeperWorker(); keepers.push(keeper); return keeper as unknown as Worker; },
    onRecovered: (report) => recoveries.push(report),
    onFailure: (reason) => failures.push(reason),
  });
  fusion.reset();
  const feed = async (from: number, to: number): Promise<void> => {
    for (let i = from; i < to; i += 1) {
      const frame = frames[i];
      for (let wait = 0; wait < 400 && !fusion.client.canAccept(true) && fusion.client.hasFailed === null; wait += 1) await new Promise<void>((resolve) => setTimeout(resolve, 2));   /* a restore is under way */
      expect(fusion.submitFrame(frame.depth, frame.view, frameContext(frame), null, 1000 + i)).toBe('accepted');
      await settle(2);                                                              /* checkpoints travel over real MessagePorts */
    }
  };
  return { fusion, workers, keepers, recoveries, failures, feed };
}

async function uninterrupted(count = FRAMES): Promise<Record<string, unknown>> {
  const r = rig();
  await r.feed(0, count);
  const result = await r.fusion.finish(count, count);
  r.fusion.dispose();
  return snapshotDigest(result.snapshot);
}

describe('checkpoint completeness', () => {
  it('a structured clone of the state, restored into a new accumulator, continues bit-identically (at 10, 50 and 90 % of a scan)', async () => {
    const reference = await uninterrupted();
    for (const cut of [6, 30, 54]) {
      const first = rig({ noKeeper: true });
      await first.feed(0, cut);
      const original = (first.workers[0].handler.core as DepthFusionCore);
      const cloned = structuredClone(original.checkpoint());
      const resumed = rig({ noKeeper: true });
      const core = resumed.workers[0].handler.core;
      core.reset(resumed.fusion.client.currentGeneration);
      core.restore({ ...cloned, generation: resumed.fusion.client.currentGeneration, throughSeq: 0 });
      await resumed.feed(cut, FRAMES);
      const result = await resumed.fusion.finish(FRAMES, FRAMES);
      expect(snapshotDigest(result.snapshot), `restored after ${cut} frames`).toEqual(reference);
    }
  }, 120_000);

  it('a state that does not match the accumulator (a field added or removed) is refused instead of restoring half a scan', () => {
    const core = new DepthFusionCore();
    const state = structuredClone(captureFusionState(core.accumulator));
    const missing = structuredClone(state); delete missing.accumulator.sampleEpoch;
    expect(() => restoreFusionState(new DepthFusionCore().accumulator, missing)).toThrow(/missing \[sampleEpoch\]/);
    const extra = structuredClone(state); extra.occupancy.surprise = 1;
    expect(() => restoreFusionState(new DepthFusionCore().accumulator, extra)).toThrow(/unknown \[surprise\]/);
    expect(() => restoreFusionState(new DepthFusionCore().accumulator, state)).not.toThrow();
  });
});

describe('the worker dies mid-scan', () => {
  for (const [label, crashAt] of [['10 %', 6], ['50 %', 30], ['90 %', 54]] as const) {
    it(`at ${label}: the replacement restores the checkpoint, fuses the journal again, and the finished scan is bit-identical; nothing is lost`, async () => {
      const reference = await uninterrupted();
      const r = rig({ onWorker: (worker, index) => { if (index === 0) worker.crashAtFrame = crashAt; } });
      await r.feed(0, FRAMES);
      const result = await r.fusion.finish(FRAMES, FRAMES);
      expect(r.workers).toHaveLength(2);
      expect(r.recoveries).toHaveLength(1);
      expect(r.recoveries[0]).toMatchObject({ lostFrames: 0, lostKeyframes: 0 });
      expect(r.recoveries[0].replayedFrames).toBeLessThanOrEqual(8 * 2 + 8);
      if (crashAt > 16) expect(r.recoveries[0].restoredThroughSeq, 'a stored checkpoint was used, not only the journal').toBeGreaterThan(0);
      expect(r.failures).toEqual([]);
      expect(snapshotDigest(result.snapshot)).toEqual(reference);
    }, 120_000);
  }

  it('during Finish (while the snapshot is being asked for): Finish still returns the complete scan', async () => {
    const reference = await uninterrupted();
    const r = rig({ onWorker: (worker, index) => { if (index === 0) worker.crashOnRequest = 'snapshot'; } });
    await r.feed(0, FRAMES);
    const result = await r.fusion.finish(FRAMES, FRAMES);
    expect(r.recoveries).toHaveLength(1);
    expect(snapshotDigest(result.snapshot)).toEqual(reference);
  }, 120_000);

  it('a HUNG worker at Finish is replaced after the bounded wait and the scan is still complete', async () => {
    const reference = await uninterrupted(30);
    const r = rig({ drainTimeoutMs: 150, requestTimeoutMs: 400 });
    await r.feed(0, 28);
    r.workers[0].silent = true;
    for (const frame of frames.slice(28, 30)) expect(r.fusion.submitFrame(frame.depth, frame.view, frameContext(frame), null, 1000 + frame.index)).toBe('accepted');
    const result = await r.fusion.finish(30, 30);
    expect(r.workers[0].terminated).toBe(true);
    expect(r.recoveries[0].lostFrames).toBe(0);
    expect(snapshotDigest(result.snapshot)).toEqual(reference);
  }, 120_000);

  it('during the scan of a Continue (a later generation), a crash before its first checkpoint never restores the PREVIOUS scan', async () => {
    const reference = await uninterrupted(6);
    const r = rig({ onWorker: (worker, index) => { if (index === 0) worker.crashAtFrame = 40 + 5; } });
    await r.feed(0, 40);                                                            /* the first scan: it leaves checkpoints in the keeper */
    await r.fusion.finish(40, 40);
    r.fusion.client.reset();                                                        /* the Continue's own scan session on the same worker */
    r.fusion.client.resumeIntake();
    await r.feed(0, 6);
    const result = await r.fusion.finish(6, 6);
    expect(r.recoveries.at(-1)).toMatchObject({ restoredThroughSeq: -1, lostFrames: 0 });
    expect(snapshotDigest(result.snapshot)).toEqual(reference);
  }, 120_000);

  it('MEMORY PRESSURE (the keeper is gone): the journal alone recovers what it still holds and the rest is REPORTED as lost, never silently', async () => {
    const r = rig({ journalLimit: 10, restoreTimeoutMs: 100, onWorker: (worker, index) => { if (index === 0) worker.crashAtFrame = 30; } });
    r.keepers[0].kill();                                                            /* it dies before the first checkpoint */
    await r.feed(0, FRAMES);
    const result = await r.fusion.finish(FRAMES, FRAMES);
    expect(r.recoveries).toHaveLength(1);
    expect(r.recoveries[0].restoredThroughSeq).toBe(-1);
    expect(r.recoveries[0].lostFrames).toBeGreaterThanOrEqual(19);
    expect(r.recoveries[0].lostKeyframes).toBeGreaterThan(0);
    expect(r.fusion.client.counters.lostFrames).toBe(r.recoveries[0].lostFrames);
    expect((result.snapshot as ScanPointCloudSnapshot).pointCount).toBeGreaterThan(0);
  }, 120_000);

  it('a keeper that dies AFTER storing checkpoints takes them with it: the frames those covered are counted as lost too', async () => {
    const r = rig({ journalLimit: 40, restoreTimeoutMs: 100, onWorker: (worker, index) => { if (index === 0) worker.crashAtFrame = 30; } });
    await r.feed(0, 20);
    expect(r.fusion.client.counters.checkpointsStored).toBeGreaterThanOrEqual(2);
    r.keepers[0].kill();
    await r.feed(20, 40);
    await r.fusion.finish(40, 40);
    const journalHeld = r.recoveries[0].replayedFrames;
    expect(r.recoveries[0].restoredThroughSeq).toBe(-1);
    expect(r.recoveries[0].lostFrames, 'every frame fused before the crash that the journal no longer held').toBe(29 - (journalHeld - 1));
    expect(r.recoveries[0].lostFrames).toBeGreaterThanOrEqual(16);
  }, 120_000);

  it('a worker that keeps dying ends in a reported, terminal failure after a bounded number of restarts', async () => {
    const r = rig({ maxRestarts: 2, onWorker: (worker) => { worker.crashAtFrame = 3; } });
    for (let i = 0; i < 12; i += 1) { const frame = frames[i]; r.fusion.submitFrame(frame.depth, frame.view, frameContext(frame), null, 1000 + i); await settle(2); }
    expect(r.workers.length).toBeLessThanOrEqual(3);
    expect(r.failures).toHaveLength(1);
    await expect(r.fusion.finish(12, 12)).rejects.toThrow(/failed/);
  });

  it('the journal is bounded and pruned by stored checkpoints (no unbounded memory on the main thread)', async () => {
    const r = rig();
    await r.feed(0, FRAMES);
    expect(r.fusion.client.counters.checkpointsStored).toBeGreaterThanOrEqual(5);
    expect(r.fusion.client.counters.maxJournal).toBeLessThanOrEqual(8 * 2 + 8);
    expect(r.fusion.client.journalSize).toBeLessThanOrEqual(8 + 1);
    expect(vi.isFakeTimers()).toBe(false);
  }, 120_000);
});
