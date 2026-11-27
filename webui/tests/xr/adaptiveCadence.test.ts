import { describe, expect, it } from 'vitest';

import { AdaptiveCadence, CADENCE_CALM_EVALUATIONS, CADENCE_EVALUATION_SPACING_MS, COVERAGE_CADENCE, LIVE_MESH_CADENCE, TAG_CADENCE, type CadenceLoad } from '../../src/xr/adaptiveCadence';

const CALM: CadenceLoad = { pressure: 'low', fusionBehind: false };
const HIGH: CadenceLoad = { pressure: 'high', fusionBehind: false };

/** Evaluates once per XR frame for `ms` of virtual time. */
function run(cadence: AdaptiveCadence, fromMs: number, ms: number, load: CadenceLoad, stepMs = 1000 / 60): number {
  let now = fromMs;
  for (; now < fromMs + ms; now += stepMs) cadence.intervalMs(now, load);
  return now;
}

describe('AdaptiveCadence', () => {
  it('runs at the base rate while nothing is wrong', () => {
    for (const options of [LIVE_MESH_CADENCE, COVERAGE_CADENCE, TAG_CADENCE]) {
      const cadence = new AdaptiveCadence(options);
      run(cadence, 0, 10_000, CALM);
      expect(cadence.currentMs).toBe(options.baseMs);
      expect(cadence.changes).toBe(0);
    }
  });

  it('follows the MEASURED cost: a task that takes longer runs less often, within its share of time, never beyond the maximum', () => {
    const cadence = new AdaptiveCadence(LIVE_MESH_CADENCE);
    for (let i = 0; i < 20; i += 1) cadence.recordCost(900);
    expect(cadence.intervalMs(0, CALM)).toBeCloseTo(900 / 0.25, 0);
    for (let i = 0; i < 30; i += 1) cadence.recordCost(5000);
    expect(cadence.intervalMs(1, CALM)).toBe(LIVE_MESH_CADENCE.maxMs);
    cadence.recordCost(Number.NaN); cadence.recordCost(-3);
    expect(cadence.measuredCostMs).toBeGreaterThan(4900);
  });

  it('backs off at once under sustained pressure, and the order of yielding is live mesh, coverage, then tags', () => {
    const factors = [LIVE_MESH_CADENCE, COVERAGE_CADENCE, TAG_CADENCE].map((options) => new AdaptiveCadence(options).intervalMs(0, HIGH) / options.baseMs);
    expect(factors[0]).toBeGreaterThan(factors[1]);
    expect(factors[1]).toBeGreaterThan(factors[2]);
    const tags = new AdaptiveCadence(TAG_CADENCE);
    expect(tags.intervalMs(0, { pressure: 'medium', fusionBehind: true }), 'tags keep their rate under medium pressure and a fusion backlog').toBe(TAG_CADENCE.baseMs);
  });

  it('yields to a fusion backlog (the fusion worker also builds the coverage and live-mesh input)', () => {
    const behind: CadenceLoad = { pressure: 'low', fusionBehind: true };
    expect(new AdaptiveCadence(LIVE_MESH_CADENCE).intervalMs(0, behind)).toBe(LIVE_MESH_CADENCE.baseMs * 2);
    expect(new AdaptiveCadence(COVERAGE_CADENCE).intervalMs(0, behind)).toBe(COVERAGE_CADENCE.baseMs * 2);
  });

  it('speeds up only after several calm evaluations spaced in time, in steps, and ends exactly at the base rate', () => {
    const cadence = new AdaptiveCadence(LIVE_MESH_CADENCE);
    cadence.intervalMs(0, HIGH);
    const slow = cadence.currentMs;
    run(cadence, 10, CADENCE_EVALUATION_SPACING_MS * (CADENCE_CALM_EVALUATIONS - 1), CALM);
    expect(cadence.currentMs, 'hundreds of frames inside a few windows are not several calm evaluations').toBe(slow);
    const afterFirstStep = run(cadence, 10 + CADENCE_EVALUATION_SPACING_MS * (CADENCE_CALM_EVALUATIONS - 1), CADENCE_EVALUATION_SPACING_MS * 1.5, CALM);
    expect(cadence.currentMs).toBeLessThan(slow);
    expect(cadence.currentMs).toBeGreaterThan(LIVE_MESH_CADENCE.baseMs);
    run(cadence, afterFirstStep, 30_000, CALM);
    expect(cadence.currentMs).toBe(LIVE_MESH_CADENCE.baseMs);
  });

  it('NO STARVATION: under the worst load that can be measured, for ten minutes, every task still runs at least at its slowest rate', () => {
    const worst: CadenceLoad = { pressure: 'high', fusionBehind: true };
    for (const [options, slowestMs] of [[LIVE_MESH_CADENCE, 6300], [COVERAGE_CADENCE, 2200], [TAG_CADENCE, 1000]] as const) {
      const cadence = new AdaptiveCadence(options);
      let lastRun = 0; let runs = 0; let longest = 0;
      for (let now = 0; now < 600_000; now += 1000 / 30) {
        if (now - lastRun >= cadence.intervalMs(now, worst)) { longest = Math.max(longest, now - lastRun); lastRun = now; runs += 1; cadence.recordCost(60_000); }
      }
      expect(cadence.currentMs).toBeLessThanOrEqual(slowestMs);
      expect(longest, 'the longest wait is the slowest interval plus one frame').toBeLessThanOrEqual(slowestMs + 34);
      expect(runs).toBeGreaterThanOrEqual(Math.floor(600_000 / (slowestMs + 34)) - 1);
    }
  });

  it('HYSTERESIS: pressure that flips every 400 ms changes the rate once, not on every flip', () => {
    const cadence = new AdaptiveCadence(LIVE_MESH_CADENCE);
    let now = 0;
    for (let flip = 0; flip < 100; flip += 1) now = run(cadence, now, 400, flip % 2 === 0 ? HIGH : CALM);
    expect(cadence.changes).toBe(1);
    expect(cadence.currentMs).toBe(LIVE_MESH_CADENCE.baseMs * 3.5);
  });

  it('a target inside the dead band around the current interval changes nothing', () => {
    const cadence = new AdaptiveCadence({ ...LIVE_MESH_CADENCE, dutyShare: 1 });
    for (let i = 0; i < 40; i += 1) cadence.recordCost(2000);
    cadence.intervalMs(0, CALM);
    const settled = cadence.currentMs; const changes = cadence.changes;
    for (let i = 0; i < 200; i += 1) { cadence.recordCost(i % 2 === 0 ? 2150 : 1850); cadence.intervalMs(i * 100, CALM); }
    expect(cadence.currentMs).toBe(settled);
    expect(cadence.changes).toBe(changes);
  });

  it('a clock that goes back (a new XR session) does not block recovery, and reset() forgets the measurements', () => {
    const cadence = new AdaptiveCadence(LIVE_MESH_CADENCE);
    cadence.intervalMs(1_000_000, HIGH);
    run(cadence, 0, 30_000, CALM);
    expect(cadence.currentMs).toBe(LIVE_MESH_CADENCE.baseMs);
    cadence.recordCost(1500); cadence.intervalMs(40_000, HIGH);
    cadence.reset();
    expect(cadence.currentMs).toBe(LIVE_MESH_CADENCE.baseMs);
    expect(cadence.measuredCostMs).toBe(0);
  });
});
