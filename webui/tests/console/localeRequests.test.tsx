import { afterEach, describe, expect, it, vi } from 'vitest';
import { createElement } from 'react';
import { act, create, type ReactTestRenderer } from 'react-test-renderer';

const api = vi.hoisted(() => ({ fetchDeviceStatus: vi.fn(), sendControl: vi.fn() }));
vi.mock('../../src/api/deviceApi', async (importOriginal) => {
  const actual = await importOriginal<typeof import('../../src/api/deviceApi')>();
  return { ...actual, ...api };
});

import DeviceConsoleApp from '../../src/DeviceConsoleApp';
import { LocaleProvider, translate, useLocale } from '../../src/i18n';
import type { I18nLocale } from '../../src/i18n';
import type { DeviceStatus } from '../../src/domain/device';

function deferred<T>() {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((done) => { resolve = done; });
  return { promise, resolve };
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

    act(() => setLocale('uk'));
    expect(api.fetchDeviceStatus).toHaveBeenCalledTimes(3);
    expect(api.sendControl).toHaveBeenCalledTimes(1);
    expect(renderer.root.findByProps({ 'aria-label': translate('uk', 'web.locale_label') }).props.value).toBe('uk');

    act(() => renderer.unmount());
  });
});
