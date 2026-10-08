import { afterEach, describe, expect, it, vi } from 'vitest';
import { createElement, useState } from 'react';
import { act, create, type ReactTestInstance, type ReactTestRenderer } from 'react-test-renderer';

const api = vi.hoisted(() => ({ fetchDeviceStatus: vi.fn(), sendControl: vi.fn() }));
vi.mock('../../src/api/deviceApi', async (importOriginal) => {
  const actual = await importOriginal<typeof import('../../src/api/deviceApi')>();
  return { ...actual, ...api };
});

import DeviceConsoleApp from '../../src/DeviceConsoleApp';
import { loadUkrainianCatalog, LocaleProvider, translate, useLocale } from '../../src/i18n';
import type { I18nLocale } from '../../src/i18n';
import type { DeviceStatus } from '../../src/domain/device';
import { jobProgressText, waitForLocalizationJob, type LocalizationJobStatus } from '../../src/domain/localizationJob';
import LocalizationJobReport from '../../src/features/objects/LocalizationJobReport';
import { formatNumber } from '../../src/i18n/format';

function deferred<T>() {
  let resolve!: (value: T) => void;
  let reject!: (reason?: unknown) => void;
  const promise = new Promise<T>((done, fail) => { resolve = done; reject = fail; });
  return { promise, resolve, reject };
}

afterEach(() => {
  vi.unstubAllGlobals();
  vi.clearAllMocks();
});

describe('presentation-only locale changes', () => {
  it('keeps the view coherent as locale changes during a status poll and job completion', async () => {
    vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
    let nextTimerId = 0;
    const timers = new Map<number, () => void>();
    const intervals = new Map<number, () => void>();
    vi.stubGlobal('window', {
      localStorage: { getItem: () => null, setItem: vi.fn() },
      setTimeout: (callback: () => void) => { const id = ++nextTimerId; timers.set(id, callback); return id; },
      clearTimeout: (id: number) => { timers.delete(id); },
      setInterval: (callback: () => void) => { const id = ++nextTimerId; intervals.set(id, callback); return id; },
      clearInterval: (id: number) => { intervals.delete(id); },
      confirm: () => false,
    });
    const extended = deferred<DeviceStatus>();
    const polling = deferred<DeviceStatus>();
    const command = deferred<{ ok: boolean; action: string; object_id?: string }>();
    api.fetchDeviceStatus.mockImplementation((includeObjects = false) => includeObjects ? extended.promise : polling.promise);
    api.sendControl.mockReturnValue(command.promise);

    const runningJob: LocalizationJobStatus = {
      state: 'running', job_id: 3, object_id: 'OBJ_0001', session_id: 'S000001', computed_at_ms: 10,
      current_method: 'tdcas', elapsed_ms: 1200, budget_ms: 60000,
      summary: {
        participating_channel_count: 4, methods_attempted: 1, methods_solved: 1, methods_persisted: 1,
        methods_skipped_for_time: 0, timings: [{ method: 'tdcas', ms: 1234, skipped_for_time: false }],
      },
      stack: { size_bytes: 8192, free_min_bytes: 4096, margin_bytes: 1024, margin_ok: true },
    };
    const completedJob = { ...runningJob, state: 'succeeded' as const, current_method: '' };
    let setLocalizationJobStatus!: (status: LocalizationJobStatus) => void;
    function LocalizationJobProbe() {
      const [status, setStatus] = useState(runningJob);
      setLocalizationJobStatus = setStatus;
      return createElement(LocalizationJobReport, { status });
    }

    let setLocale!: (locale: I18nLocale) => void;
    function LocaleProbe() {
      setLocale = useLocale().setLocale;
      return null;
    }
    let renderer!: ReactTestRenderer;
    await act(async () => {
      renderer = create(createElement(LocaleProvider, null,
        createElement(DeviceConsoleApp),
        createElement(LocaleProbe),
        createElement(LocalizationJobProbe),
      ));
    });
    expect(api.fetchDeviceStatus).toHaveBeenCalledTimes(1);
    expect(api.fetchDeviceStatus).toHaveBeenLastCalledWith(true);

    const objectsTab = renderer.root.findAllByType('button').find((button) => button.children.join('') === translate('en', 'nav.objects'));
    expect(objectsTab).toBeDefined();
    act(() => objectsTab?.props.onClick());
    expect(api.fetchDeviceStatus).toHaveBeenCalledTimes(2);

    // Start a real status read before submitting: periodic reads are suppressed while a control is pending.
    const pollEntry = timers.entries().next().value as [number, () => void] | undefined;
    const poll = pollEntry?.[1];
    expect(poll).toBeDefined();
    await act(async () => {
      if (pollEntry !== undefined) timers.delete(pollEntry[0]);
      poll?.();
    });
    expect(api.fetchDeviceStatus).toHaveBeenCalledTimes(3);
    expect(api.fetchDeviceStatus).toHaveBeenLastCalledWith(false);

    const form = renderer.root.findByType('form');
    act(() => form.props.onSubmit({ preventDefault: () => undefined }));
    expect(api.sendControl).toHaveBeenCalledTimes(1);
    expect(api.sendControl).toHaveBeenCalledWith('create_object', {});

    await act(async () => { poll?.(); });
    expect(api.fetchDeviceStatus).toHaveBeenCalledTimes(3);

    const localizationJobPoll = deferred<LocalizationJobStatus>();
    const fetchLocalizationJobStatus = vi.fn(() => localizationJobPoll.promise);
    const localizationJobWait = waitForLocalizationJob(fetchLocalizationJobStatus, {
      objectId: 'OBJ_0001', sessionId: 'S000001', budgetMs: 60000,
      onProgress: setLocalizationJobStatus, sleep: async () => undefined, now: () => 0,
    });
    expect(fetchLocalizationJobStatus).toHaveBeenCalledTimes(1);
    await act(async () => {
      setLocale('uk');
      localizationJobPoll.resolve(completedJob);
      await localizationJobWait;
      await loadUkrainianCatalog();
    });
    expect(api.fetchDeviceStatus).toHaveBeenCalledTimes(3);
    expect(api.sendControl).toHaveBeenCalledTimes(1);
    expect(renderer.root.findByProps({ 'aria-label': translate('uk', 'web.locale_label') }).props.value).toBe('uk');
    const report = renderer.root.findByProps({ className: 'localization-job' });
    expect(report.props['aria-label']).toBe(translate('uk', 'calc.job_aria'));
    expect(report.findAllByType('p')[0].props['data-job-state']).toBe('succeeded');
    expect(report.findAllByType('p')[0].children.join('')).toBe(jobProgressText(completedJob, 'uk'));
    /* A ReactTestInstance has no toJSON(): collect every rendered string in the subtree. */
    const textOf = (node: ReactTestInstance | string): string =>
      typeof node === 'string' ? node : node.children.map(textOf).join(' ');
    const reportJson = textOf(report);
    expect(reportJson).toContain(translate('uk', 'calc.job_measured', { count: formatNumber('uk', 1) }));
    expect(reportJson).toContain(formatNumber('uk', 1234));
    for (const key of ['calc.job_method', 'calc.job_time', 'calc.job_share'] as const) {
      expect(reportJson).toContain(translate('uk', key));
      expect(reportJson).not.toContain(translate('en', key));
    }
    expect(reportJson).not.toContain(translate('en', 'calc.job_measured', { count: formatNumber('en', 1) }));
    expect(fetchLocalizationJobStatus).toHaveBeenCalledTimes(1);
    expect(api.fetchDeviceStatus).toHaveBeenCalledTimes(3);
    expect(api.sendControl).toHaveBeenCalledTimes(1);

    act(() => renderer.unmount());
  });

  it('retranslates a visible poll error when locale changes without another API call', async () => {
    vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
    let nextTimerId = 0;
    const timers = new Map<number, () => void>();
    vi.stubGlobal('window', {
      localStorage: { getItem: () => null, setItem: vi.fn() },
      setTimeout: (callback: () => void) => { const id = ++nextTimerId; timers.set(id, callback); return id; },
      clearTimeout: (id: number) => { timers.delete(id); },
      setInterval: () => ++nextTimerId,
      clearInterval: vi.fn(),
      confirm: () => false,
    });
    const extended = deferred<DeviceStatus>();
    const polling = deferred<DeviceStatus>();
    api.fetchDeviceStatus.mockImplementation((includeObjects = false) => includeObjects ? extended.promise : polling.promise);

    let setLocale!: (locale: I18nLocale) => void;
    function LocaleProbe() {
      setLocale = useLocale().setLocale;
      return null;
    }
    let renderer!: ReactTestRenderer;
    await act(async () => {
      renderer = create(createElement(LocaleProvider, null,
        createElement(DeviceConsoleApp),
        createElement(LocaleProbe),
      ));
    });
    expect(api.fetchDeviceStatus).toHaveBeenCalledTimes(1);

    const poll = timers.values().next().value as (() => void) | undefined;
    expect(poll).toBeDefined();
    await act(async () => {
      poll?.();
      polling.reject(new Error('ESP API network error (GET /api/status): offline'));
      await polling.promise.catch(() => undefined);
    });
    expect(api.fetchDeviceStatus).toHaveBeenCalledTimes(2);
    const englishError = translate('en', 'api.error_request_failed', { detail: 'GET /api/status: offline' });
    expect(JSON.stringify(renderer.toJSON())).toContain(englishError);

    await act(async () => {
      setLocale('uk');
      await loadUkrainianCatalog();
    });
    const ukrainianError = translate('uk', 'api.error_request_failed', { detail: 'GET /api/status: offline' });
    expect(JSON.stringify(renderer.toJSON())).toContain(ukrainianError);
    expect(api.fetchDeviceStatus).toHaveBeenCalledTimes(2);
    expect(api.sendControl).not.toHaveBeenCalled();

    extended.resolve({
      system: {
        state: 'READY',
        mode: 'NORMAL',
        uptime_ms: 1,
        acquisition_ready: true,
        ready_pods: 4,
        required_pods: 4,
      },
      power: { valid: true, voltage_v: 5, current_a: 0, power_w: 0 },
      environment: { valid: true, temperature_c: 20, humidity_percent: 50, pressure_pa: 100000 },
      mux: { valid: true, active_channel: 1, active_pod: 1 },
      pods: [],
      acquisition: {
        running: false,
        requested_running: false,
        hardware_ready: true,
        source: 'AD7606',
        last_error: 0,
        last_error_name: 'ESP_OK',
        sample_rate_hz: 500,
        samples_recorded: 0,
        recording_ms: 0,
      },
      measurement: { available: true, state: 'IDLE', busy: false, object_id: 'OBJ_0001' },
    });
    await act(async () => { await extended.promise; });
    act(() => renderer.unmount());
  });
});
