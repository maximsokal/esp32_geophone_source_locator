import { describe, expect, it, vi } from 'vitest';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

import { translate } from '../../src/i18n';
import { formatNumber, formatPercent } from '../../src/i18n/format';
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

const seconds = (ms: number): string => `${formatNumber('en', ms / 1000, { minimumFractionDigits: 1, maximumFractionDigits: 1 })} s`;

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
    })).rejects.toThrow(translate('en', 'calc.job_timeout', {
      duration: seconds(5000 + JOB_WAIT_GRACE_MS),
      lastPoll: translate('en', 'calc.job_last_poll', { error: 'HTTP 503' }),
    }));
    expect(clock).toBeGreaterThanOrEqual(5000 + JOB_WAIT_GRACE_MS);
  });
});

describe('what the panel shows', () => {
  it('describes every state from the device fields', () => {
    expect(jobProgressText(status({ state: 'queued' }), 'en')).toBe(translate('en', 'calc.job_queued'));
    expect(jobProgressText(status({ state: 'running', current_method: 'bayesian_scan_voids', elapsed_ms: 12345 }), 'en'))
      .toBe(translate('en', 'calc.job_running', {
        method: 'bayesian_scan_voids', elapsed: seconds(12345), budget: seconds(60000),
      }));
    expect(jobProgressText(status(), 'en')).toBe(translate('en', 'calc.job_done', {
      elapsed: seconds(8350), solved: formatNumber('en', 13), attempted: formatNumber('en', 18),
    }));
    const skipped = status();
    skipped.summary = { ...skipped.summary, methods_skipped_for_time: 2 };
    expect(jobProgressText(skipped, 'en')).toBe(translate('en', 'calc.job_done', {
      elapsed: seconds(8350), solved: formatNumber('en', 13), attempted: formatNumber('en', 18),
    }) + translate('en', 'calc.job_skipped', { count: formatNumber('en', 2), budget: seconds(60000) }));
    expect(jobProgressText(status({ state: 'failed', error: 'ESP_ERR_NOT_FOUND', elapsed_ms: 200 }), 'en'))
      .toBe(translate('en', 'calc.job_failed', { elapsed: seconds(200), error: 'ESP_ERR_NOT_FOUND' }));
  });

  it('shows the measured stack headroom and flags a thin margin', () => {
    expect(stackText(status(), 'en')).toBe(translate('en', 'calc.stack_summary', {
      used: '7.0 KiB', total: '16.0 KiB', free: '9.0 KiB',
    }));
    const thin = status({ stack: { size_bytes: 16384, free_min_bytes: 2048, margin_bytes: 4096, margin_ok: false } });
    expect(stackText(thin, 'en')).toBe(translate('en', 'calc.stack_summary', {
      used: '14.0 KiB', total: '16.0 KiB', free: '2.0 KiB',
    }) + translate('en', 'calc.stack_below', { margin: '4.0 KiB' }));
    expect(stackText(status({ stack: { size_bytes: 16384, free_min_bytes: 0, margin_bytes: 4096, margin_ok: true } }), 'en')).toBe('');
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
    expect(running).toContain(jobProgressText(status({ state: 'running', current_method: 'xcorr_tdoa', elapsed_ms: 1500 }), 'en'));
    expect(running).not.toContain(translate('en', 'calc.job_measured', { count: formatNumber('en', 3) }));

    const done = status();
    done.summary = {
      ...done.summary,
      methods_skipped_for_time: 1,
      timings: [...done.summary.timings, { method: 'bayesian_scan_voids', ms: 0, skipped_for_time: true }],
    };
    const html = renderToStaticMarkup(createElement(LocalizationJobReport, { status: done }));
    expect(html).toContain(`<summary>${translate('en', 'calc.job_measured', { count: formatNumber('en', 4) })}</summary>`);
    expect(html).toContain(`<td>bayesian</td><td>${formatNumber('en', 2400)} ms</td><td>${formatPercent('en', 2400 / 4003)}</td>`);
    expect(html).toContain(`<td>bayesian_scan_voids</td><td>${translate('en', 'calc.job_not_started')}</td><td>—</td>`);
    expect(html).toContain(translate('en', 'calc.stack_summary', {
      used: '7.0 KiB', total: '16.0 KiB', free: '9.0 KiB',
    }));
    expect(html).not.toContain('role="alert"');

    const thin = renderToStaticMarkup(createElement(LocalizationJobReport, {
      status: status({ stack: { size_bytes: 16384, free_min_bytes: 1024, margin_bytes: 4096, margin_ok: false } }),
    }));
    expect(thin).toContain('role="alert"');
    expect(thin).toContain(translate('en', 'calc.stack_below', { margin: '4.0 KiB' }));
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
    const text = uncertaintyText({ uncertainty: { ...u, sigma_inflation: 21 } }, 'en');
    expect(text).toBe(`±${formatNumber('en', 0.1, { minimumFractionDigits: 2, maximumFractionDigits: 2 })} / ${formatNumber('en', 0.1, { minimumFractionDigits: 2, maximumFractionDigits: 2 })} / ${formatNumber('en', 0.3, { minimumFractionDigits: 2, maximumFractionDigits: 2 })} m (${translate('en', 'overlay.one_sd')}) · ${translate('en', 'overlay.region_percent', { percent: formatPercent('en', 0.9) })} ${formatNumber('en', 0.12, { minimumFractionDigits: 2, maximumFractionDigits: 2 })} m³ · ${translate('en', 'overlay.widened_misfit', { factor: formatNumber('en', 21, { minimumFractionDigits: 1, maximumFractionDigits: 1 })})}`);
    expect(uncertaintyText({ uncertainty: { ...u, sigma_inflation: 1 } }, 'en')).not.toContain(translate('en', 'overlay.widened_misfit', { factor: '1.0' }));
    expect(uncertaintyText({ uncertainty: u }, 'en')).not.toContain(translate('en', 'overlay.widened_misfit', { factor: '1.0' }));
  });
});
