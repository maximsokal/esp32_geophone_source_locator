import { describe, expect, it } from 'vitest';

import { LiveMeshStats, type LiveMeshStatsEvent } from '../../src/xr/liveMeshStats';

const collect = () => { const events: LiveMeshStatsEvent[] = []; return { events, stats: new LiveMeshStats((e) => events.push(e), 10_000) }; };
const context = { mode: 'auto' };

describe('aggregated live mesh statistics (TASK 11)', () => {
  it('emits nothing before the interval has passed, and nothing at all when nothing happened', () => {
    const { events, stats } = collect();
    stats.maybeFlush(0, context);
    stats.recordBuild(40, 856, 143);
    stats.maybeFlush(9_999, context);
    expect(events).toEqual([]);
    const idle = collect();
    idle.stats.maybeFlush(0, context); idle.stats.maybeFlush(10_000, context); idle.stats.maybeFlush(20_000, context);
    expect(idle.events).toEqual([]);
  });

  it('one BUILD and one RENDER record per interval, with the distribution of all builds and frames, not just the first', () => {
    const { events, stats } = collect();
    stats.maybeFlush(0, context);
    for (let i = 1; i <= 100; i += 1) stats.recordBuild(i, 1000 + i, 140 + i);               /* 1..100 ms */
    for (let i = 1; i <= 200; i += 1) stats.recordFrame(i / 100);
    stats.recordUpload(0.4); stats.recordUpload(0.9);
    stats.maybeFlush(10_000, context);
    expect(events.map((e) => e.event)).toEqual(['XR_LIVE_MESH_BUILD_STATS', 'XR_LIVE_MESH_RENDER_STATS']);
    expect(events[0].fields).toMatchObject({ mode: 'auto', intervalMs: 10_000, builds: 100, buildMsP50: 50, buildMsP95: 95, buildMsMax: 100, triangles: 1100, maxTriangles: 1100, points: 240 });
    expect(events[1].fields).toMatchObject({ frames: 200, renderMsP50: 1, renderMsP95: 1.9, renderMsMax: 2, uploads: 2, uploadMsP95: 0.9, triangles: 1100 });
  });

  it('starts a fresh interval after each flush (no sample is reported twice)', () => {
    const { events, stats } = collect();
    stats.maybeFlush(0, context);
    stats.recordBuild(10, 100, 50);
    stats.maybeFlush(10_000, context);
    expect(events).toHaveLength(1);
    stats.maybeFlush(20_000, context);
    expect(events).toHaveLength(1);
    stats.recordBuild(20, 200, 60);
    stats.maybeFlush(30_000, context);
    expect(events).toHaveLength(2);
    expect(events[1].fields).toMatchObject({ builds: 1, buildMsP50: 20, triangles: 200 });
  });

  it('bounds its memory: a very busy interval keeps the most recent 512 samples and still counts every build', () => {
    const { events, stats } = collect();
    stats.maybeFlush(0, context);
    for (let i = 0; i < 5000; i += 1) stats.recordBuild(i < 4000 ? 1 : 100, 10, 10);
    stats.maybeFlush(10_000, context);
    expect(events[0].fields.builds).toBe(5000);
    expect(events[0].fields.buildMsP50).toBe(100);                                          /* only the last 512 (all 100 ms) remain */
  });

  it('an explicit flush at the end of a session reports what is left', () => {
    const { events, stats } = collect();
    stats.maybeFlush(0, context);
    stats.recordFrame(2);
    stats.flush(1_500, context);
    expect(events.map((e) => e.event)).toEqual(['XR_LIVE_MESH_RENDER_STATS']);
    expect(events[0].fields.intervalMs).toBe(1500);
  });
});
