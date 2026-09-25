import { describe, expect, it, vi } from 'vitest';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

import {
  JOB_WAIT_GRACE_MS,
  jobProgressText,
  jobTimingRows,
  stackText,
  waitForLocalizationJob,
  type LocalizationJobStatus,
} from '../../src/domain/localizationJob';
import { uncertaintyText } from '../../src/domain/localizationOverlay';
import LocalizationJobReport from '../../src/features/objects/LocalizationJobReport';
import { timeBudgetSeconds } from '../../src/features/objects/LocalizationCalculatePanel';

/*
 * "Calculate source" as a background job: the WebUI queues it, polls the
 * device's status, and shows the device's own measurements (per-method
 * time, worker stack high-water mark) -- nothing is timed in the browser.
 */

/* Exactly the firmware's JSON (components/localization_job). */
const FIRMWARE_DONE = '{"state":"succeeded","job_id":7,"object_id":"OBJ_0001","session_id":"S000001",' +
  '"computed_at_ms":42,"current_method":"","elapsed_ms":8350,"budget_ms":60000,' +
  '"summary":{"participating_channel_count":4,"methods_attempted":18,"methods_solved":13,"methods_persisted":21,' +
  '"methods_skipped_for_time":0,"timings":[{"method":"tdoa_gauss_newton","ms":3,"skipped_for_time":false},' +
  '{"method":"bayesian","ms":2400,"skipped_for_time":false},{"method":"eikonal_fmm","ms":1600,"skipped_for_time":false}]},' +
  '"stack":{"size_bytes":16384,"free_min_bytes":9216,"margin_bytes":4096,"margin_ok":true}}';

function status(overrides: Partial<LocalizationJobStatus> = {}): LocalizationJobStatus {
  return { ...(JSON.parse(FIRMWARE_DONE) as LocalizationJobStatus), ...overrides };
}

describe('waiting for the device job', () => {
  it('reports progress and returns the finished status of this session', async () => {
    const sequence: LocalizationJobStatus[] = [
      status({ state: 'queued', elapsed_ms: 0 }),
      status({ session_id: 'S000009', state: 'succeeded' }), // someone else's job: ignored
      status({ state: 'running', current_method: 'eikonal_scan_voids', elapsed_ms: 4200 }),
      status(),
    ];
    const fetchStatus = vi.fn(async () => sequence.shift()!);
    const seen: string[] = [];
    const sleep = vi.fn(async () => undefined);
    const done = await waitForLocalizationJob(fetchStatus, {
      objectId: 'OBJ_0001', sessionId: 'S000001', budgetMs: 60000,
      onProgress: (s) => seen.push(s.state), sleep, now: () => 0,
    });
    expect(done.state).toBe('succeeded');
    expect(fetchStatus).toHaveBeenCalledTimes(4);
    expect(seen).toEqual(['queued', 'running', 'succeeded']);
    expect(sleep).toHaveBeenCalledTimes(3);
  });

  it('retries a failed poll and gives up only after budget + grace', async () => {
    let clock = 0;
    const fetchStatus = vi.fn(async () => { throw new Error('HTTP 503'); });
    const sleep = vi.fn(async (ms: number) => { clock += ms; });
    await expect(waitForLocalizationJob(fetchStatus, {
      objectId: 'OBJ_0001', sessionId: 'S000001', budgetMs: 5000, sleep, now: () => clock,
    })).rejects.toThrow(/did not finish the calculation within 35\.0 s \(last poll: HTTP 503\)/);
    expect(clock).toBeGreaterThanOrEqual(5000 + JOB_WAIT_GRACE_MS);
  });
});

describe('what the panel shows', () => {
  it('describes every state from the device fields', () => {
    expect(jobProgressText(status({ state: 'queued' }))).toBe('Queued on the device…');
    expect(jobProgressText(status({ state: 'running', current_method: 'bayesian_scan_voids', elapsed_ms: 12345 })))
      .toBe('Calculating on the device: bayesian_scan_voids · 12.3 s of 60.0 s budget');
    expect(jobProgressText(status())).toBe('Done in 8.3 s: 13 of 18 methods solved');
    const skipped = status();
    skipped.summary = { ...skipped.summary, methods_skipped_for_time: 2 };
    expect(jobProgressText(skipped)).toMatch(/, 2 not started \(time budget 60\.0 s\)$/);
    expect(jobProgressText(status({ state: 'failed', error: 'ESP_ERR_NOT_FOUND', elapsed_ms: 200 })))
      .toBe('Failed after 0.2 s: ESP_ERR_NOT_FOUND');
  });

  it('shows the measured stack headroom and flags a thin margin', () => {
    expect(stackText(status())).toBe('worker stack: 7.0 KiB used at most of 16.0 KiB (min free 9.0 KiB)');
    const thin = status({ stack: { size_bytes: 16384, free_min_bytes: 2048, margin_bytes: 4096, margin_ok: false } });
    expect(stackText(thin)).toMatch(/below the 4\.0 KiB safety margin$/);
    expect(stackText(status({ stack: { size_bytes: 16384, free_min_bytes: 0, margin_bytes: 4096, margin_ok: true } }))).toBe('');
  });

  it('lists timings slowest first with their share', () => {
    const rows = jobTimingRows(status());
    expect(rows.map((row) => row.method)).toEqual(['bayesian', 'eikonal_fmm', 'tdoa_gauss_newton']);
    expect(rows[0].share).toBeCloseTo(2400 / 4003, 12);
  });

  it('renders progress while running and the measurements when done', () => {
    const running = renderToStaticMarkup(createElement(LocalizationJobReport, {
      status: status({ state: 'running', current_method: 'xcorr_tdoa', elapsed_ms: 1500 }),
    }));
    expect(running).toContain('data-job-state="running"');
    expect(running).toContain('Calculating on the device: xcorr_tdoa · 1.5 s of 60.0 s budget');
    expect(running).not.toContain('Measured on the device');

    const done = status();
    done.summary = {
      ...done.summary,
      methods_skipped_for_time: 1,
      timings: [...done.summary.timings, { method: 'bayesian_scan_voids', ms: 0, skipped_for_time: true }],
    };
    const html = renderToStaticMarkup(createElement(LocalizationJobReport, { status: done }));
    expect(html).toContain('Measured on the device (4 steps)');
    expect(html).toContain('<td>bayesian</td><td>2400 ms</td><td>60%</td>');
    expect(html).toContain('<td>bayesian_scan_voids</td><td>not started (time budget)</td>');
    expect(html).toContain('worker stack: 7.0 KiB used at most');
    expect(html).not.toContain('role="alert"');

    const thin = renderToStaticMarkup(createElement(LocalizationJobReport, {
      status: status({ stack: { size_bytes: 16384, free_min_bytes: 1024, margin_bytes: 4096, margin_ok: false } }),
    }));
    expect(thin).toContain('role="alert"');
  });

  it('validates the optional time budget', () => {
    expect(timeBudgetSeconds('')).toBeUndefined();
    expect(timeBudgetSeconds('  ')).toBeUndefined();
    expect(timeBudgetSeconds('45.5')).toBe(45.5);
    expect(timeBudgetSeconds('0.5')).toBeNull();
    expect(timeBudgetSeconds('601')).toBeNull();
    expect(timeBudgetSeconds('abc')).toBeNull();
  });

  it('says when a Bayesian region was widened by the misfit check', () => {
    const u = {
      std_m: { x_m: 0.1, y_m: 0.1, z_m: 0.3 }, credible_level: 0.9, credible_volume_m3: 0.12, credible_area_m2: 0,
      credible_min: { x_m: 0, y_m: 0, z_m: -1 }, credible_max: { x_m: 1, y_m: 1, z_m: 0 },
    };
    expect(uncertaintyText({ uncertainty: { ...u, sigma_inflation: 21 } })).toMatch(/ · widened ×21\.0 \(data misfit\)$/);
    expect(uncertaintyText({ uncertainty: { ...u, sigma_inflation: 1 } })).not.toContain('widened');
    expect(uncertaintyText({ uncertainty: u })).not.toContain('widened');
  });
});
