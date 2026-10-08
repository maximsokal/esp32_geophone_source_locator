import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createElement } from 'react';
import { act, create, type ReactTestInstance, type ReactTestRenderer } from 'react-test-renderer';

const api = vi.hoisted(() => ({ fetchDeviceStatus: vi.fn(), sendControl: vi.fn() }));
vi.mock('../../src/api/deviceApi', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../src/api/deviceApi')>()),
  ...api,
}));

import DeviceConsoleApp from '../../src/DeviceConsoleApp';
import DeviceDashboard from '../../src/features/dashboard/DeviceDashboard';
import ObjectsFeature from '../../src/features/objects/ObjectsFeature';
import { DeviceStatusValidationError } from '../../src/api/deviceStatusSchema';
import type { DeviceStatus } from '../../src/domain/device';
import { LocaleProvider, translate } from '../../src/i18n';

const mounted: ReactTestRenderer[] = [];

function deviceStatus(options: {
  storageAvailable?: boolean;
  systemState?: string;
  step?: string;
  measurementState?: string;
} = {}): DeviceStatus {
  return {
    system: { state: options.systemState ?? 'READY', mode: 'NORMAL', uptime_ms: 1, acquisition_ready: true, ready_pods: 4, required_pods: 4 },
    power: { valid: true, voltage_v: 12, current_a: 0, power_w: 0 },
    environment: { valid: true, temperature_c: 20, humidity_percent: 50, pressure_pa: 100000 },
    mux: { valid: true, active_channel: 1, active_pod: 1 },
    pods: [],
    acquisition: { running: false, requested_running: false, hardware_ready: true, source: 'AD7606', sample_rate_hz: 500, samples_recorded: 0, recording_ms: 0 },
    storage_available: options.storageAvailable ?? true,
    object_store: {
      available: true,
      active_id: 'OBJ_0001',
      default_cable_lengths_m: [5, 5, 5, 5],
      objects: [{
        id: 'OBJ_0001', name: 'Bridge pier', active: true,
        location: { valid: false, latitude: 0, longitude: 0, accuracy_m: 0, altitude_valid: false, altitude_m: 0, altitude_accuracy_m: 0, captured_at_ms: 0, source: '' },
        cable_lengths_m: [5, 5, 5, 5],
      }],
    },
    ...(options.step === undefined ? {} : { workflow: { step: options.step } }),
    ...(options.measurementState === undefined ? {} : { measurement: { available: true, state: options.measurementState, busy: false, object_id: 'OBJ_0001' } }),
  } as unknown as DeviceStatus;
}

interface ScheduledTimeout {
  callback: () => void;
  delay: number;
}

interface BrowserStub {
  timers: Map<number, ScheduledTimeout>;
  intervals: Map<number, () => void>;
  timeoutCalls: ScheduledTimeout[];
  document: {
    hidden: boolean;
    visibilityState: string;
    addEventListener: (type: string, listener: () => void) => void;
    removeEventListener: (type: string, listener: () => void) => void;
  };
  navigator: { onLine: boolean };
  dispatchDocument: (type: string) => void;
  dispatchWindow: (type: string) => void;
  listenerCount: (target: 'document' | 'window', type: string) => number;
  addCount: (target: 'document' | 'window', type: string) => number;
  removeCount: (target: 'document' | 'window', type: string) => number;
  windowAddEventListener: (type: string, listener: () => void) => void;
}

function stubBrowser(): BrowserStub {
  let nextId = 0;
  const timers = new Map<number, ScheduledTimeout>();
  const intervals = new Map<number, () => void>();
  const timeoutCalls: ScheduledTimeout[] = [];
  const documentListeners = new Map<string, Set<() => void>>();
  const windowListeners = new Map<string, Set<() => void>>();
  const documentAdds = new Map<string, number>();
  const documentRemoves = new Map<string, number>();
  const windowAdds = new Map<string, number>();
  const windowRemoves = new Map<string, number>();
  const updateCount = (counts: Map<string, number>, type: string): void => {
    counts.set(type, (counts.get(type) ?? 0) + 1);
  };
  const addListener = (listeners: Map<string, Set<() => void>>, counts: Map<string, number>, type: string, listener: () => void): void => {
    const bucket = listeners.get(type) ?? new Set<() => void>();
    bucket.add(listener);
    listeners.set(type, bucket);
    updateCount(counts, type);
  };
  const removeListener = (listeners: Map<string, Set<() => void>>, counts: Map<string, number>, type: string, listener: () => void): void => {
    listeners.get(type)?.delete(listener);
    updateCount(counts, type);
  };
  const document = {
    hidden: false,
    visibilityState: 'visible',
    addEventListener: (type: string, listener: () => void) => addListener(documentListeners, documentAdds, type, listener),
    removeEventListener: (type: string, listener: () => void) => removeListener(documentListeners, documentRemoves, type, listener),
  };
  const navigator = { onLine: true };
  const windowAddEventListener = (type: string, listener: () => void): void => addListener(windowListeners, windowAdds, type, listener);
  const window = {
    localStorage: { getItem: () => null, setItem: vi.fn() },
    setTimeout: (callback: () => void, delay = 0) => {
      const scheduled = { callback, delay };
      const id = ++nextId;
      timers.set(id, scheduled);
      timeoutCalls.push(scheduled);
      return id;
    },
    clearTimeout: (id: number) => { timers.delete(id); },
    setInterval: (callback: () => void) => {
      const id = ++nextId;
      intervals.set(id, callback);
      return id;
    },
    clearInterval: (id: number) => { intervals.delete(id); },
    confirm: () => false,
    addEventListener: windowAddEventListener,
    removeEventListener: (type: string, listener: () => void) => removeListener(windowListeners, windowRemoves, type, listener),
  };
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  vi.stubGlobal('window', window);
  vi.stubGlobal('document', document);
  vi.stubGlobal('navigator', navigator);
  return {
    timers,
    intervals,
    timeoutCalls,
    document,
    navigator,
    dispatchDocument: (type) => { for (const listener of documentListeners.get(type) ?? []) listener(); },
    dispatchWindow: (type) => { for (const listener of windowListeners.get(type) ?? []) listener(); },
    listenerCount: (target, type) => (target === 'document' ? documentListeners : windowListeners).get(type)?.size ?? 0,
    addCount: (target, type) => (target === 'document' ? documentAdds : windowAdds).get(type) ?? 0,
    removeCount: (target, type) => (target === 'document' ? documentRemoves : windowRemoves).get(type) ?? 0,
    windowAddEventListener,
  };
}

function textOf(node: ReactTestInstance | string): string {
  return typeof node === 'string' ? node : node.children.map(textOf).join('');
}

function unmount(renderer: ReactTestRenderer): void {
  act(() => renderer.unmount());
  const index = mounted.indexOf(renderer);
  if (index >= 0) mounted.splice(index, 1);
}

function findButton(renderer: ReactTestRenderer, label: string): ReactTestInstance {
  const button = renderer.root.findAllByType('button').find((candidate) => textOf(candidate) === label);
  if (button === undefined) throw new Error(`Expected button: ${label}`);
  return button;
}

async function settleReact(): Promise<void> {
  await Promise.resolve();
  await Promise.resolve();
  await Promise.resolve();
}

async function mountApp(): Promise<ReactTestRenderer> {
  let renderer!: ReactTestRenderer;
  await act(async () => {
    renderer = create(createElement(LocaleProvider, { initialLocale: 'en' }, createElement(DeviceConsoleApp)));
    await settleReact();
  });
  mounted.push(renderer);
  return renderer;
}

function nextTimer(browser: BrowserStub): ScheduledTimeout | undefined {
  const entry = browser.timers.entries().next().value as [number, ScheduledTimeout] | undefined;
  return entry?.[1];
}

async function fireNextTimer(browser: BrowserStub): Promise<void> {
  const entry = browser.timers.entries().next().value as [number, ScheduledTimeout] | undefined;
  if (entry === undefined) throw new Error('Expected a scheduled status poll.');
  browser.timers.delete(entry[0]);
  await act(async () => {
    entry[1].callback();
    await settleReact();
  });
}

async function advanceOneCadence(browser: BrowserStub): Promise<void> {
  const due = [...browser.timers.entries()];
  browser.timers.clear();
  await act(async () => {
    for (const [, timer] of due) timer.callback();
    await settleReact();
  });
}

async function setHidden(browser: BrowserStub, hidden: boolean): Promise<void> {
  browser.document.hidden = hidden;
  browser.document.visibilityState = hidden ? 'hidden' : 'visible';
  await act(async () => { browser.dispatchDocument('visibilitychange'); await settleReact(); });
}

function fastStatusReadCount(): number {
  return api.fetchDeviceStatus.mock.calls.filter(([includeObjects, sessionsObjectId]) => includeObjects !== true && sessionsObjectId === undefined).length;
}

beforeEach(() => {
  api.fetchDeviceStatus.mockReset().mockImplementation((includeObjects = false) => Promise.resolve(deviceStatus()));
  api.sendControl.mockReset().mockResolvedValue({ ok: true, action: 'noop' });
});

afterEach(() => {
  for (const renderer of [...mounted]) unmount(renderer);
  vi.unstubAllGlobals();
  vi.clearAllMocks();
});

describe('DeviceConsoleApp status polling', () => {
  // Expected RED on old code: the old poll does not pause while the document is hidden.
  it('keeps baseline cadence, pauses hidden polling, and performs one refresh on visibility resume', async () => {
    const browser = stubBrowser();
    api.fetchDeviceStatus.mockResolvedValue(deviceStatus({ step: 'IDLE' }));
    const renderer = await mountApp();

    expect(browser.timeoutCalls[0]?.delay).toBe(5000);
    expect(nextTimer(browser)?.delay).toBe(5000);
    expect(api.fetchDeviceStatus).toHaveBeenCalledTimes(1);
    expect(api.fetchDeviceStatus).toHaveBeenLastCalledWith(true);

    await setHidden(browser, true);
    expect(browser.timers.size).toBe(0);
    const readsWhileHidden = fastStatusReadCount();
    for (let cadence = 0; cadence < 8; cadence += 1) {
      await advanceOneCadence(browser);
      expect(browser.timers.size).toBe(0);
      expect(fastStatusReadCount()).toBe(readsWhileHidden);
    }

    await setHidden(browser, false);
    expect(fastStatusReadCount()).toBe(readsWhileHidden + 1);
    expect(browser.timers.size).toBe(1);
    expect(nextTimer(browser)?.delay).toBe(5000);
    unmount(renderer);
    expect(browser.timers.size).toBe(0);
    expect(browser.listenerCount('document', 'visibilitychange')).toBe(0);
  });

  // Expected RED on old code: workflow cadence changes currently rebuild the poll effect, and no environment listeners exist.
  it('rearms the poll at the fast workflow cadence without duplicating listeners', async () => {
    const browser = stubBrowser();
    api.fetchDeviceStatus.mockResolvedValue(deviceStatus({ step: 'CALIBRATING' }));
    const renderer = await mountApp();

    expect(browser.timeoutCalls[0]?.delay).toBe(5000);
    expect(nextTimer(browser)?.delay).toBe(1000);
    expect(browser.addCount('document', 'visibilitychange')).toBe(1);
    expect(browser.addCount('window', 'online')).toBe(1);
    expect(browser.addCount('window', 'offline')).toBe(1);

    unmount(renderer);
    expect(browser.removeCount('document', 'visibilitychange')).toBe(1);
    expect(browser.removeCount('window', 'online')).toBe(1);
    expect(browser.removeCount('window', 'offline')).toBe(1);
  });

  // Expected RED on old code: no visibilitychange/online/offline subscriptions or in-flight resume coalescing exist.
  it('coalesces rapid visibility and online changes around one deferred poll', async () => {
    const browser = stubBrowser();
    let resolvePoll!: (status: DeviceStatus) => void;
    let currentReads = 0;
    let maximumReads = 0;
    api.fetchDeviceStatus.mockImplementation((includeObjects = false) => {
      if (includeObjects) return Promise.resolve(deviceStatus());
      currentReads += 1;
      maximumReads = Math.max(maximumReads, currentReads);
      return new Promise<DeviceStatus>((resolve) => {
        resolvePoll = (status) => {
          currentReads -= 1;
          resolve(status);
        };
      });
    });
    const renderer = await mountApp();

    await setHidden(browser, true);
    await setHidden(browser, false);
    expect(fastStatusReadCount()).toBe(1);
    for (let cycle = 0; cycle < 10; cycle += 1) {
      await setHidden(browser, true);
      expect(browser.timers.size).toBeLessThanOrEqual(1);
      await setHidden(browser, false);
      expect(browser.timers.size).toBeLessThanOrEqual(1);
    }
    browser.navigator.onLine = false;
    await act(async () => { browser.dispatchWindow('offline'); await settleReact(); });
    browser.navigator.onLine = true;
    await act(async () => { browser.dispatchWindow('online'); await settleReact(); });

    expect(fastStatusReadCount()).toBe(1);
    expect(maximumReads).toBe(1);
    expect(browser.addCount('document', 'visibilitychange')).toBe(1);
    expect(browser.removeCount('document', 'visibilitychange')).toBe(0);
    expect(browser.listenerCount('document', 'visibilitychange')).toBe(1);
    expect(browser.timers.size).toBeLessThanOrEqual(1);

    await act(async () => { resolvePoll(deviceStatus()); await settleReact(); });
    expect(browser.timers.size).toBe(1);
    unmount(renderer);
    expect(browser.addCount('document', 'visibilitychange')).toBe(browser.removeCount('document', 'visibilitychange'));
    expect(browser.addCount('window', 'online')).toBe(browser.removeCount('window', 'online'));
    expect(browser.addCount('window', 'offline')).toBe(browser.removeCount('window', 'offline'));
    expect(browser.timers.size).toBe(0);
  });

  // Expected RED on old code: offline and combined hidden/offline states do not gate status reads.
  it('gates reads while offline or hidden and resumes once when both conditions allow polling', async () => {
    const browser = stubBrowser();
    const renderer = await mountApp();

    browser.navigator.onLine = false;
    await act(async () => { browser.dispatchWindow('offline'); await settleReact(); });
    expect(browser.timers.size).toBe(0);
    const readsOffline = fastStatusReadCount();
    browser.navigator.onLine = true;
    await act(async () => { browser.dispatchWindow('online'); await settleReact(); });
    expect(fastStatusReadCount()).toBe(readsOffline + 1);
    expect(browser.timers.size).toBe(1);

    await setHidden(browser, true);
    const readsHidden = fastStatusReadCount();
    browser.navigator.onLine = false;
    await act(async () => { browser.dispatchWindow('offline'); await settleReact(); });
    browser.navigator.onLine = true;
    await act(async () => { browser.dispatchWindow('online'); await settleReact(); });
    expect(fastStatusReadCount()).toBe(readsHidden);
    expect(browser.timers.size).toBe(0);

    await setHidden(browser, false);
    expect(fastStatusReadCount()).toBe(readsHidden + 1);
    expect(browser.timers.size).toBe(1);
    unmount(renderer);
  });

  // Expected RED on old code: there is no online resume read while preserving the failed poll's backoff.
  it('keeps failure backoff and makes one prompt online refresh after an offline pause', async () => {
    const browser = stubBrowser();
    api.fetchDeviceStatus.mockImplementation((includeObjects = false) => includeObjects
      ? Promise.resolve(deviceStatus())
      : Promise.reject(new Error('network unavailable')));
    await mountApp();

    await fireNextTimer(browser);
    expect(fastStatusReadCount()).toBe(1);
    expect(nextTimer(browser)?.delay).toBe(10000);
    browser.navigator.onLine = false;
    await act(async () => { browser.dispatchWindow('offline'); await settleReact(); });
    expect(browser.timers.size).toBe(0);
    browser.navigator.onLine = true;
    await act(async () => { browser.dispatchWindow('online'); await settleReact(); });
    expect(fastStatusReadCount()).toBe(2);
    expect(browser.timers.size).toBe(1);
    expect(nextTimer(browser)?.delay).toBe(20000);
  });

  // Expected RED on old code: the periodic tick issues a status request while a write is unresolved.
  it('suppresses polling during a control and uses its post-control status read exactly once', async () => {
    const browser = stubBrowser();
    let resolveControl!: (value: { ok: boolean; action: string }) => void;
    api.fetchDeviceStatus.mockResolvedValue(deviceStatus());
    api.sendControl.mockReturnValue(new Promise((resolve) => { resolveControl = resolve; }));
    const renderer = await mountApp();
    const objectsTab = renderer.root.findAllByType('button').find((button) => textOf(button) === translate('en', 'nav.objects'));
    await act(async () => { objectsTab?.props.onClick(); await settleReact(); });
    const objects = renderer.root.findByType(ObjectsFeature);
    const callsBeforeControl = api.fetchDeviceStatus.mock.calls.length;
    let controlPromise!: Promise<void>;
    await act(async () => {
      controlPromise = objects.props.onAcquisition(true);
      await Promise.resolve();
    });

    await fireNextTimer(browser);
    expect(api.fetchDeviceStatus).toHaveBeenCalledTimes(callsBeforeControl);
    expect(browser.timers.size).toBe(1);

    await act(async () => {
      resolveControl({ ok: true, action: 'acquisition_start' });
      await controlPromise;
      await settleReact();
    });
    expect(fastStatusReadCount()).toBe(1);
    expect(api.fetchDeviceStatus).toHaveBeenCalledTimes(callsBeforeControl + 1);
    expect(browser.timers.size).toBe(1);
  });

  // Expected RED on old code: a tick during measurement_start reads again beside its transition poll.
  it('uses the measurement transition read to satisfy a suppressed poll without an extra fast refresh', async () => {
    const browser = stubBrowser();
    let resolveControl!: (value: { ok: boolean; action: string }) => void;
    api.fetchDeviceStatus.mockImplementation((includeObjects = false, sessionsObjectId?: string) => {
      if (includeObjects) return Promise.resolve(deviceStatus());
      if (sessionsObjectId !== undefined) return Promise.resolve(deviceStatus());
      return Promise.resolve(deviceStatus({ measurementState: 'RECORDING' }));
    });
    api.sendControl.mockReturnValue(new Promise((resolve) => { resolveControl = resolve; }));
    const renderer = await mountApp();
    const objectsTab = renderer.root.findAllByType('button').find((button) => textOf(button) === translate('en', 'nav.objects'));
    await act(async () => { objectsTab?.props.onClick(); await settleReact(); });
    const objects = renderer.root.findByType(ObjectsFeature);
    const callsBeforeControl = api.fetchDeviceStatus.mock.calls.length;
    let controlPromise!: Promise<void>;
    await act(async () => {
      controlPromise = objects.props.onRecording('OBJ_0001', true);
      await Promise.resolve();
    });

    await fireNextTimer(browser);
    expect(api.fetchDeviceStatus).toHaveBeenCalledTimes(callsBeforeControl);
    await act(async () => {
      resolveControl({ ok: true, action: 'measurement_start' });
      await controlPromise;
      await settleReact();
    });

    // measurement_start needs no session read; the earlier OBJ_0001 read came from opening Objects.
    // The suppressed tick is satisfied by exactly this transition status read.
    expect(api.fetchDeviceStatus.mock.calls.slice(callsBeforeControl)).toEqual([[false]]);
    expect(browser.timers.size).toBe(1);
  });

  // Regression guard for last-good retention and recovery; only the bounded-message assertions are RED on old code.
  it('retains the last known status on malformed data and bounds the visible validation message', async () => {
    const browser = stubBrowser();
    const initialStatus = deviceStatus({ systemState: 'STARTUP' });
    const firstStatus = deviceStatus({ systemState: 'READY' });
    const nextStatus = deviceStatus({ systemState: 'CALIBRATING' });
    let fastReads = 0;
    api.fetchDeviceStatus.mockImplementation((includeObjects = false) => {
      if (includeObjects) return Promise.resolve(initialStatus);
      fastReads += 1;
      if (fastReads === 1) return Promise.resolve(firstStatus);
      if (fastReads === 2) return Promise.reject(new DeviceStatusValidationError('workflow'.repeat(100), 'invalid field '.repeat(100)));
      return Promise.resolve(nextStatus);
    });
    const renderer = await mountApp();
    expect(renderer.root.findByType(DeviceDashboard).props.status).toBe(initialStatus);

    await fireNextTimer(browser);
    expect(renderer.root.findByType(DeviceDashboard).props.status).toBe(firstStatus);

    await fireNextTimer(browser);
    const afterFailure = renderer.root.findByType(DeviceDashboard);
    expect(afterFailure.props.status).toBe(firstStatus);
    expect(afterFailure.props.error).toContain('Invalid device status');
    expect((afterFailure.props.error as string).length).toBeLessThanOrEqual(200);
    expect(textOf(renderer.root.findByProps({ role: 'alert' })).length).toBeLessThanOrEqual(240);
    expect(JSON.stringify(renderer.toJSON())).toContain(translate('en', 'console.api_error'));

    await fireNextTimer(browser);
    const afterRecovery = renderer.root.findByType(DeviceDashboard);
    expect(afterRecovery.props.status).toBe(nextStatus);
    expect(afterRecovery.props.error).toBeNull();
    expect(JSON.stringify(renderer.toJSON())).toContain(translate('en', 'console.api_link'));
  });

  // Expected RED on old code: a good fast status does not clear validation failures stored in workspaceError.
  it('clears a prior session validation failure after a later valid status read', async () => {
    const browser = stubBrowser();
    const emptyWorkspace = deviceStatus();
    if (emptyWorkspace.object_store !== undefined) {
      emptyWorkspace.object_store.active_id = null;
      emptyWorkspace.object_store.objects = [];
    }
    let badSession = true;
    api.fetchDeviceStatus.mockImplementation((includeObjects = false, sessionsObjectId?: string) => {
      if (sessionsObjectId !== undefined && badSession) {
        badSession = false;
        return Promise.reject(new DeviceStatusValidationError('recording_sessions.sessions', 'missing required field'));
      }
      return Promise.resolve(includeObjects ? emptyWorkspace : deviceStatus());
    });
    const renderer = await mountApp();
    const objectsTab = renderer.root.findAllByType('button').find((button) => textOf(button) === translate('en', 'nav.objects'));
    await act(async () => { objectsTab?.props.onClick(); await settleReact(); });
    const objects = renderer.root.findByType(ObjectsFeature);
    const lastGoodStatus = objects.props.status;
    await act(async () => { objects.props.onSelect('OBJ_0001'); await settleReact(); });
    expect(renderer.root.findByType(ObjectsFeature).props.status).toBe(lastGoodStatus);
    expect(renderer.root.findAllByProps({ role: 'alert' }).length).toBeGreaterThan(0);
    expect(JSON.stringify(renderer.toJSON())).toContain(translate('en', 'console.api_error'));

    await fireNextTimer(browser);
    expect(renderer.root.findByType(ObjectsFeature).props.error).toBeNull();
    expect(JSON.stringify(renderer.toJSON())).toContain(translate('en', 'console.api_link'));
  });

  // Expected RED on old code: objects polling currently reads even while hidden and has no online gate.
  it('skips objects interval ticks while hidden and clears all listeners and timers on unmount', async () => {
    const browser = stubBrowser();
    const renderer = await mountApp();
    const objectsTab = renderer.root.findAllByType('button').find((button) => textOf(button) === translate('en', 'nav.objects'));
    await act(async () => { objectsTab?.props.onClick(); await settleReact(); });
    expect(browser.intervals.size).toBe(1);
    const interval = browser.intervals.values().next().value as (() => void) | undefined;
    expect(interval).toBeDefined();
    const extendedReadsBeforeHidden = api.fetchDeviceStatus.mock.calls.filter(([includeObjects]) => includeObjects === true).length;
    await setHidden(browser, true);
    await act(async () => { interval?.(); await settleReact(); });
    expect(api.fetchDeviceStatus.mock.calls.filter(([includeObjects]) => includeObjects === true)).toHaveLength(extendedReadsBeforeHidden);
    await setHidden(browser, false);
    expect(api.fetchDeviceStatus.mock.calls.filter(([includeObjects]) => includeObjects === true)).toHaveLength(extendedReadsBeforeHidden);

    unmount(renderer);
    expect(browser.timers.size).toBe(0);
    expect(browser.intervals.size).toBe(0);
    for (const type of ['visibilitychange', 'online', 'offline']) {
      const target = type === 'visibilitychange' ? 'document' : 'window';
      expect(browser.addCount(target, type)).toBe(browser.removeCount(target, type));
    }
  });
});
