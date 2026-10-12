import { afterEach, describe, expect, it, vi } from 'vitest';
import { createElement } from 'react';
import { act, create, type ReactTestInstance, type ReactTestRenderer } from 'react-test-renderer';

const api = vi.hoisted(() => ({ fetchDeviceStatus: vi.fn(), sendControl: vi.fn() }));
const scannerModule = vi.hoisted(() => ({ attempts: 0, mode: 'fail' as 'fail' | 'success' }));

vi.mock('../../src/api/deviceApi', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../src/api/deviceApi')>()),
  ...api,
}));

vi.mock('../../src/App', () => {
  scannerModule.attempts += 1;
  if (scannerModule.mode === 'fail') throw new Error('simulated scanner chunk failure');
  return {
    default: () => createElement('main', { 'data-testid': 'mocked-scanner' }, 'Scanner ready'),
  };
});

import DeviceConsoleApp from '../../src/DeviceConsoleApp';
import ObjectsFeature from '../../src/features/objects/ObjectsFeature';
import WorkflowPanel from '../../src/features/workflow/WorkflowPanel';
import ScannerFeatureHost, {
  SCANNER_MAX_IN_PLACE_RETRIES,
  SCANNER_RELOAD_SESSION_KEY,
  type ScannerFeatureHostProps,
  type ScannerFeatureProps,
  type ScannerSessionStorage,
} from '../../src/features/scanner/ScannerFeatureHost';
import type { DeviceStatus } from '../../src/domain/device';
import { LocaleProvider, translate } from '../../src/i18n';

const mounted: ReactTestRenderer[] = [];

function deviceStatus(storageAvailable = true): DeviceStatus {
  return {
    system: { state: 'READY', mode: 'NORMAL', uptime_ms: 1, acquisition_ready: true, ready_pods: 4, required_pods: 4 },
    power: { valid: true, voltage_v: 12, current_a: 0, power_w: 0 },
    environment: { valid: true, temperature_c: 20, humidity_percent: 50, pressure_pa: 100000 },
    mux: { valid: true, active_channel: 1, active_pod: 1 },
    pods: [],
    acquisition: { running: false, requested_running: false, hardware_ready: true, source: 'AD7606', sample_rate_hz: 500, samples_recorded: 0, recording_ms: 0 },
    storage_available: storageAvailable,
    workflow: { step: 'IDLE' },
    object_store: {
      available: true,
      active_id: 'OBJ_0001',
      default_cable_lengths_m: [5, 5, 5, 5],
      objects: [],
    },
  } as unknown as DeviceStatus;
}

interface BrowserStub {
  timers: Map<number, { callback: () => void; delay: number }>;
  intervals: Map<number, () => void>;
  window: Record<string, unknown>;
}

function stubBrowser(): BrowserStub {
  let nextId = 0;
  const timers = new Map<number, { callback: () => void; delay: number }>();
  const intervals = new Map<number, () => void>();
  const window = {
    localStorage: { getItem: () => null, setItem: vi.fn() },
    location: { reload: vi.fn() },
    setTimeout: (callback: () => void, delay = 0) => {
      const id = ++nextId;
      timers.set(id, { callback, delay });
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
    addEventListener: vi.fn(),
    removeEventListener: vi.fn(),
  };
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  vi.stubGlobal('window', window);
  vi.stubGlobal('document', {
    hidden: false,
    visibilityState: 'visible',
    addEventListener: vi.fn(),
    removeEventListener: vi.fn(),
  });
  vi.stubGlobal('navigator', { onLine: true });
  return { timers, intervals, window };
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

async function mountConsole(): Promise<ReactTestRenderer> {
  let renderer!: ReactTestRenderer;
  await act(async () => {
    renderer = create(createElement(LocaleProvider, { initialLocale: 'en' }, createElement(DeviceConsoleApp)));
    await settleReact();
  });
  mounted.push(renderer);
  return renderer;
}

function scannerProps(overrides: Partial<ScannerFeatureHostProps> = {}): ScannerFeatureHostProps {
  return {
    objectId: null,
    sessionId: null,
    status: null,
    onBack: vi.fn(),
    onDashboard: vi.fn(),
    ...overrides,
  };
}

function failingLoader() {
  return vi.fn(async () => {
    throw new Error('temporary scanner import failure');
  });
}

afterEach(() => {
  for (const renderer of [...mounted]) unmount(renderer);
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
  vi.clearAllMocks();
  scannerModule.attempts = 0;
  scannerModule.mode = 'fail';
});

describe('DeviceConsoleApp scanner chunk boundary', () => {
  it('resumes a workflow step from the device snapshot after a browser reload', async () => {
    stubBrowser();
    const status = deviceStatus(false);
    status.workflow = {
      step: 'GPS_PENDING',
      generation: 44,
      allowed_actions: ['GPS_SKIP', 'BACK'],
      gps_status: 'pending',
      object_id: 'OBJ_0001',
    } as DeviceStatus['workflow'];
    api.fetchDeviceStatus.mockResolvedValue(status);

    const renderer = await mountConsole();
    expect(renderer.root.findByType(WorkflowPanel).props.status.workflow.step).toBe('GPS_PENDING');
    expect(renderer.root.findByType(WorkflowPanel).props.status.workflow.generation).toBe(44);
    expect(renderer.root.findAllByType('button').some((button) => textOf(button) === translate('en', 'workflow.action.gps_skip'))).toBe(true);
  });

  it('opens the phone scanner when a fresh device snapshot enters SCANNING', async () => {
    stubBrowser();
    const choice = deviceStatus(false);
    choice.workflow = {
      step: 'SCAN_CHOICE',
      generation: 70,
      allowed_actions: ['SCAN_START', 'SCAN_SKIP', 'BACK'],
      object_id: 'OBJ_0001',
      session_id: null,
    } as DeviceStatus['workflow'];
    const scanning = deviceStatus(false);
    scanning.workflow = {
      step: 'SCANNING',
      generation: 71,
      allowed_actions: ['BACK'],
      object_id: 'OBJ_0001',
      session_id: null,
    } as DeviceStatus['workflow'];
    let current = choice;
    api.fetchDeviceStatus.mockImplementation(async () => current);

    const renderer = await mountConsole();
    expect(renderer.root.findByType(WorkflowPanel).props.status.workflow.step).toBe('SCAN_CHOICE');
    current = scanning;
    await act(async () => {
      await renderer.root.findByType(WorkflowPanel).props.onRefreshStatus();
      await settleReact();
    });

    expect(renderer.root.findByType(ScannerFeatureHost).props).toMatchObject({
      objectId: 'OBJ_0001',
      sessionId: null,
    });
  });

  it('returns a staged workflow scan to the workflow with a generation-checked BACK command', async () => {
    stubBrowser();
    const scanning = deviceStatus(false);
    scanning.workflow = {
      step: 'SCANNING',
      generation: 51,
      allowed_actions: ['BACK'],
      object_id: 'OBJ_0001',
      session_id: null,
    } as DeviceStatus['workflow'];
    api.fetchDeviceStatus.mockResolvedValue(scanning);
    const fetchMock = vi.fn().mockResolvedValue(new Response(JSON.stringify({
      ok: true,
      workflow: {
        step: 'SCAN_CHOICE',
        generation: 52,
        allowed_actions: ['SCAN_START', 'SCAN_SKIP', 'BACK'],
        object_id: 'OBJ_0001',
      },
    }), { status: 200 }));
    vi.stubGlobal('fetch', fetchMock);

    const renderer = await mountConsole();
    expect(renderer.root.findByType(ScannerFeatureHost).props).toMatchObject({
      objectId: 'OBJ_0001',
      sessionId: null,
    });
    await act(async () => {
      renderer.root.findByProps({ className: 'console-floating-back' }).props.onClick();
      await settleReact();
      await settleReact();
    });

    expect(JSON.parse(fetchMock.mock.calls[0][1]?.body as string)).toMatchObject({
      action: 'workflow_command',
      command: 'BACK',
      generation: 51,
    });
    expect(renderer.root.findByType(WorkflowPanel).props.status.workflow.step).toBe('SCAN_CHOICE');
    expect(renderer.root.findByType(WorkflowPanel).props.status.workflow.generation).toBe(52);
  });

  it('marks the staged scan complete and returns to RECORD_READY after upload', async () => {
    stubBrowser();
    const scanning = deviceStatus(false);
    scanning.workflow = {
      step: 'SCANNING',
      generation: 61,
      allowed_actions: ['BACK'],
      object_id: 'OBJ_0001',
      session_id: null,
    } as DeviceStatus['workflow'];
    api.fetchDeviceStatus.mockResolvedValue(scanning);
    const fetchMock = vi.fn().mockResolvedValue(new Response(JSON.stringify({
      ok: true,
      workflow: {
        step: 'RECORD_READY',
        generation: 62,
        allowed_actions: ['START_RECORDING', 'MOVE_SENSORS', 'BACK'],
        object_id: 'OBJ_0001',
      },
    }), { status: 200 }));
    vi.stubGlobal('fetch', fetchMock);

    const renderer = await mountConsole();
    const onScanSaved = renderer.root.findByType(ScannerFeatureHost).props.onScanSaved as () => Promise<void>;
    await act(async () => {
      await onScanSaved();
      await settleReact();
    });

    expect(JSON.parse(fetchMock.mock.calls[0][1]?.body as string)).toMatchObject({
      action: 'workflow_command',
      command: 'SCAN_COMPLETE',
      generation: 61,
    });
    expect(renderer.root.findByType(WorkflowPanel).props.status.workflow.step).toBe('RECORD_READY');
    expect(renderer.root.findByType(WorkflowPanel).props.status.workflow.generation).toBe(62);
  });

  // Expected RED on old code: a rejected lazy import escapes the root instead of rendering an in-place failure.
  it('keeps the scanner shell and console state alive, then retries with a fresh import', async () => {
    const browser = stubBrowser();
    const status = deviceStatus(false);
    api.fetchDeviceStatus.mockResolvedValue(status);
    scannerModule.attempts = 0;
    scannerModule.mode = 'fail';

    const renderer = await mountConsole();
    const objectsTab = renderer.root.findAllByType('button').find((button) => textOf(button) === translate('en', 'nav.objects'));
    expect(objectsTab).toBeDefined();
    await act(async () => {
      objectsTab?.props.onClick();
      await settleReact();
    });

    const objectsBeforeScanner = renderer.root.findByType(ObjectsFeature);
    const statusBeforeScanner = objectsBeforeScanner.props.status;
    const storeBeforeScanner = objectsBeforeScanner.props.store;
    const sessionsBeforeScanner = objectsBeforeScanner.props.sessions;
    await act(async () => {
      objectsBeforeScanner.props.onOpenScanner('OBJ_0001', 'S000001');
      await settleReact();
    });

    expect(scannerModule.attempts).toBe(1);
    expect(renderer.root.findByProps({ className: 'scanner-feature-shell' })).toBeDefined();
    expect(renderer.root.findByProps({ className: 'console-floating-back' })).toBeDefined();
    expect(renderer.root.findAllByProps({ role: 'status' }).some((node) => textOf(node).includes('SD'))).toBe(true);
    expect(renderer.root.findByProps({ role: 'alert' })).toBeDefined();
    expect(renderer.root.findByType(ScannerFeatureHost).props).toMatchObject({
      objectId: 'OBJ_0001',
      sessionId: 'S000001',
      status: statusBeforeScanner,
    });
    expect(findButton(renderer, translate('en', 'seismic.retry'))).toBeDefined();
    expect(findButton(renderer, translate('en', 'console.back_object'))).toBeDefined();
    expect(findButton(renderer, translate('en', 'nav.home'))).toBeDefined();
    expect(browser.timers.size).toBe(1);
    const docAdds = (globalThis.document.addEventListener as ReturnType<typeof vi.fn>).mock.calls.length;
    const windowAdds = (browser.window.addEventListener as ReturnType<typeof vi.fn>).mock.calls.length;
    const intervalCount = browser.intervals.size;
    for (let retry = 0; retry < SCANNER_MAX_IN_PLACE_RETRIES; retry += 1) {
      await act(async () => {
        findButton(renderer, translate('en', 'seismic.retry')).props.onClick();
        await settleReact();
      });
    }
    expect(scannerModule.attempts).toBe(SCANNER_MAX_IN_PLACE_RETRIES + 1);
    expect(renderer.root.findByProps({ role: 'alert' })).toBeDefined();
    expect(renderer.root.findAllByType('button').some((button) => textOf(button) === translate('en', 'seismic.retry'))).toBe(false);
    expect(findButton(renderer, 'Reload page')).toBeDefined();
    expect(browser.timers.size).toBe(1);
    expect(browser.intervals.size).toBe(intervalCount);
    expect((globalThis.document.addEventListener as ReturnType<typeof vi.fn>).mock.calls).toHaveLength(docAdds);
    expect((browser.window.addEventListener as ReturnType<typeof vi.fn>).mock.calls).toHaveLength(windowAdds);

    const failureBack = renderer.root.findByProps({ role: 'alert' }).findAllByType('button')
      .find((button) => textOf(button) === translate('en', 'console.back_object'));
    expect(failureBack).toBeDefined();
    await act(async () => {
      failureBack?.props.onClick();
      await settleReact();
    });
    const objectsAfterBack = renderer.root.findByType(ObjectsFeature);
    expect(objectsAfterBack.props.status).toBe(statusBeforeScanner);
    expect(objectsAfterBack.props.store).toBe(storeBeforeScanner);
    expect(objectsAfterBack.props.sessions).toBe(sessionsBeforeScanner);
    expect(objectsAfterBack.props.selectedId).toBe('OBJ_0001');
    expect(browser.window.location).toMatchObject({ reload: expect.any(Function) });
    expect((browser.window.location as { reload: ReturnType<typeof vi.fn> }).reload).not.toHaveBeenCalled();
    expect(browser.timers.size).toBe(1);

    await act(async () => {
      objectsAfterBack.props.onOpenScanner('OBJ_0001', 'S000001');
      await settleReact();
    });
    expect(scannerModule.attempts).toBe(SCANNER_MAX_IN_PLACE_RETRIES + 2);
    scannerModule.mode = 'success';
    await act(async () => {
      findButton(renderer, translate('en', 'seismic.retry')).props.onClick();
      await settleReact();
    });
    expect(scannerModule.attempts).toBe(SCANNER_MAX_IN_PLACE_RETRIES + 3);
    expect(renderer.root.findByProps({ 'data-testid': 'mocked-scanner' })).toBeDefined();
    expect(browser.timers.size).toBe(1);
  });

  // Regression guard for the new host contract; the old app has no host retry limit to exercise.
  it('bounds manual retries and offers one guarded, user-initiated reload', async () => {
    const browser = stubBrowser();
    const stored = new Map<string, string>();
    const sessionStorage: ScannerSessionStorage = {
      getItem: (key) => stored.get(key) ?? null,
      setItem: (key, value) => { stored.set(key, value); },
      removeItem: (key) => { stored.delete(key); },
    };
    const loadScanner = failingLoader();
    const reloadPage = vi.fn();
    let renderer!: ReactTestRenderer;
    await act(async () => {
      renderer = create(createElement(LocaleProvider, { initialLocale: 'en' }, createElement(ScannerFeatureHost, {
        ...scannerProps(), loadScanner, reloadPage, sessionStorage,
      })));
      await settleReact();
    });
    mounted.push(renderer);

    for (let retry = 0; retry < SCANNER_MAX_IN_PLACE_RETRIES; retry += 1) {
      await act(async () => {
        findButton(renderer, translate('en', 'seismic.retry')).props.onClick();
        await settleReact();
      });
    }

    expect(loadScanner).toHaveBeenCalledTimes(SCANNER_MAX_IN_PLACE_RETRIES + 1);
    expect(renderer.root.findAllByType('button').some((button) => textOf(button) === translate('en', 'seismic.retry'))).toBe(false);
    expect(findButton(renderer, 'Reload page')).toBeDefined();
    expect(browser.timers.size).toBe(0);
    expect(reloadPage).not.toHaveBeenCalled();

    await act(async () => { findButton(renderer, 'Reload page').props.onClick(); });
    expect(stored.get(SCANNER_RELOAD_SESSION_KEY)).toBe('1');
    expect(reloadPage).toHaveBeenCalledTimes(1);
    expect(renderer.root.findAllByType('button').map(textOf)).toEqual([translate('en', 'console.back_object')]);

    unmount(renderer);
    let guardedRenderer!: ReactTestRenderer;
    await act(async () => {
      guardedRenderer = create(createElement(LocaleProvider, { initialLocale: 'en' }, createElement(ScannerFeatureHost, {
        ...scannerProps(), loadScanner: failingLoader(), reloadPage, sessionStorage,
      })));
      await settleReact();
    });
    mounted.push(guardedRenderer);
    expect(guardedRenderer.root.findAllByType('button').map(textOf)).toEqual([translate('en', 'console.back_object')]);
    expect(reloadPage).toHaveBeenCalledTimes(1);

    const successfulLoader = vi.fn(async () => ({
      default: (props: ScannerFeatureProps) => createElement('main', { 'data-testid': 'host-scanner' }, props.objectId ?? 'none'),
    }));
    unmount(guardedRenderer);
    let successfulRenderer!: ReactTestRenderer;
    await act(async () => {
      successfulRenderer = create(createElement(LocaleProvider, { initialLocale: 'en' }, createElement(ScannerFeatureHost, {
        ...scannerProps({ objectId: 'OBJ_0001' }), loadScanner: successfulLoader, reloadPage, sessionStorage,
      })));
      await settleReact();
    });
    mounted.push(successfulRenderer);
    expect(successfulRenderer.root.findByProps({ 'data-testid': 'host-scanner' })).toBeDefined();
    expect(stored.has(SCANNER_RELOAD_SESSION_KEY)).toBe(false);
  });

  // Regression guard for private browsing: sessionStorage access failures must not escape the host.
  it('keeps the failure UI usable when sessionStorage throws', async () => {
    const browser = stubBrowser();
    const sessionStorage: ScannerSessionStorage = {
      getItem: () => { throw new Error('storage denied'); },
      setItem: () => { throw new Error('storage denied'); },
      removeItem: () => { throw new Error('storage denied'); },
    };
    const reloadPage = vi.fn();
    let renderer!: ReactTestRenderer;
    await act(async () => {
      renderer = create(createElement(LocaleProvider, { initialLocale: 'en' }, createElement(ScannerFeatureHost, {
        ...scannerProps(), loadScanner: failingLoader(), reloadPage, sessionStorage,
      })));
      await settleReact();
    });
    mounted.push(renderer);

    for (let retry = 0; retry < SCANNER_MAX_IN_PLACE_RETRIES; retry += 1) {
      await act(async () => {
        findButton(renderer, translate('en', 'seismic.retry')).props.onClick();
        await settleReact();
      });
    }
    expect(renderer.root.findByProps({ role: 'alert' })).toBeDefined();
    expect(browser.timers.size).toBe(0);
    await act(async () => { findButton(renderer, 'Reload page').props.onClick(); });
    expect(reloadPage).toHaveBeenCalledTimes(1);
    expect(renderer.root.findAllByType('button').map(textOf)).toEqual([translate('en', 'console.back_object')]);
  });
});
