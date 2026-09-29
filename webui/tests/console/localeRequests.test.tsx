import { afterEach, describe, expect, it, vi } from 'vitest';
import { createElement } from 'react';
import { act, create, type ReactTestRenderer } from 'react-test-renderer';

const api = vi.hoisted(() => ({ fetchDeviceStatus: vi.fn(), sendControl: vi.fn() }));
vi.mock('../../src/api/deviceApi', async (importOriginal) => {
  const actual = await importOriginal<typeof import('../../src/api/deviceApi')>();
  return { ...actual, ...api };
});

import DeviceConsoleApp from '../../src/DeviceConsoleApp';
import { loadUkrainianCatalog, LocaleProvider, translate, useLocale } from '../../src/i18n';
import type { I18nLocale } from '../../src/i18n';
import type { DeviceStatus } from '../../src/domain/device';

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
  it('does not duplicate a pending status poll or control command', async () => {
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
    expect(api.fetchDeviceStatus).toHaveBeenLastCalledWith(true);

    const objectsTab = renderer.root.findAllByType('button').find((button) => button.children.join('') === translate('en', 'nav.objects'));
    expect(objectsTab).toBeDefined();
    act(() => objectsTab?.props.onClick());
    expect(api.fetchDeviceStatus).toHaveBeenCalledTimes(2);

    const form = renderer.root.findByType('form');
    act(() => form.props.onSubmit({ preventDefault: () => undefined }));
    expect(api.sendControl).toHaveBeenCalledTimes(1);
    expect(api.sendControl).toHaveBeenCalledWith('create_object', {});

    const poll = timers.values().next().value as (() => void) | undefined;
    expect(poll).toBeDefined();
    await act(async () => { poll?.(); });
    expect(api.fetchDeviceStatus).toHaveBeenCalledTimes(3);
    expect(api.fetchDeviceStatus).toHaveBeenLastCalledWith(false);

    await act(async () => {
      setLocale('uk');
      await loadUkrainianCatalog();
    });
    expect(api.fetchDeviceStatus).toHaveBeenCalledTimes(3);
    expect(api.sendControl).toHaveBeenCalledTimes(1);
    expect(renderer.root.findByProps({ 'aria-label': translate('uk', 'web.locale_label') }).props.value).toBe('uk');

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
