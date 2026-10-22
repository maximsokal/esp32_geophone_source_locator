import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createElement, Fragment, useState, type ReactTestInstance } from 'react';
import { act, create, type ReactTestRenderer } from 'react-test-renderer';

const api = vi.hoisted(() => ({
  fetchDeviceStatus: vi.fn(),
  sendControl: vi.fn(),
  fetchLocalizationRuns: vi.fn(),
  fetchScanInfo: vi.fn(),
  loadSessionRegistration: vi.fn(),
  fetchSavedSeismicSession: vi.fn(),
  loadObjectScanArtifact: vi.fn(),
}));
vi.mock('../../src/api/deviceApi', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../src/api/deviceApi')>()),
  ...api,
}));

import DeviceConsoleApp from '../../src/DeviceConsoleApp';
import ObjectsFeature from '../../src/features/objects/ObjectsFeature';
import PowerBanner from '../../src/components/PowerBanner';
import type {
  DeviceStatus,
  ObjectProfile,
  ObjectStoreState,
  RecordingSessionCollection,
  RecordingSessionSummary,
} from '../../src/domain/device';
import { loadUkrainianCatalog, LocaleProvider, translate, useLocale, type I18nKey, type I18nLocale } from '../../src/i18n';
import { formatNumber } from '../../src/i18n/format';

const OBJECT_A = 'OBJ_0001';
const OBJECT_B = 'OBJ_0002';
const SESSION_A = 'S000001';
const SESSION_B = 'S000002';
const mounted: ReactTestRenderer[] = [];

function deferred<T>() {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((done) => { resolve = done; });
  return { promise, resolve };
}

function object(id: string, name: string, active: boolean): ObjectProfile {
  return {
    id, name, active,
    location: { valid: false, latitude: 0, longitude: 0, accuracy_m: 0, altitude_valid: false, altitude_m: 0, altitude_accuracy_m: 0, captured_at_ms: 0, source: '' },
    cable_lengths_m: [5, 5, 5, 5],
  };
}

function objectStore(): ObjectStoreState {
  return {
    available: true,
    active_id: OBJECT_A,
    default_cable_lengths_m: [5, 5, 5, 5],
    objects: [object(OBJECT_A, 'Bridge pier', true), object(OBJECT_B, 'Stone arch', false)],
  };
}

function session(sessionId: string): RecordingSessionSummary {
  return {
    session_id: sessionId, status: 'complete', complete: true, readable: true, source: 'AD7606',
    sample_rate_hz: 500, channel_count: 4, samples_written: 12000, dropped_frames: 0, recording_ms: 24000,
  };
}

function sessions(objectId: string, sessionId: string): RecordingSessionCollection {
  return { object_id: objectId, available: true, sessions: [session(sessionId)] };
}

function deviceStatus(overrides: Partial<DeviceStatus> = {}): DeviceStatus {
  const base = {
    system: { state: 'READY', mode: 'NORMAL', uptime_ms: 1, acquisition_ready: true, ready_pods: 4, required_pods: 4 },
    power: { valid: true, voltage_v: 12, current_a: 0, power_w: 0, state: 'BATTERY', battery_percent_valid: true, battery_percent_estimate: 42, low_battery: 'normal' },
    environment: { valid: true, temperature_c: 20, humidity_percent: 50, pressure_pa: 100000 },
    mux: { valid: true, active_channel: 1, active_pod: 1 },
    pods: [],
    acquisition: { running: false, requested_running: false, hardware_ready: true, source: 'AD7606', last_error: 0, last_error_name: 'ESP_OK', sample_rate_hz: 500, samples_recorded: 0, recording_ms: 0 },
    object_store: objectStore(),
  };
  return {
    ...base,
    ...overrides,
    power: { ...base.power, ...overrides.power },
    acquisition: { ...base.acquisition, ...overrides.acquisition },
  } as unknown as DeviceStatus;
}

function powerUpdate(lowBattery: 'grace' | 'shutdown', seconds?: number) {
  return {
    valid: true, voltage_v: 3.2, current_a: 0, power_w: 0, state: 'BATTERY_CRITICAL', low_battery: lowBattery,
    battery_percent_valid: true, battery_percent_estimate: 8,
    ...(seconds === undefined ? {} : { shutdown_in_s: seconds }),
  };
}

function recordingUpdate(power: ReturnType<typeof powerUpdate>): DeviceStatus {
  const fields = {
    available: true, running: true, state: 'RECORDING', object_id: OBJECT_A, session_id: 'S000007',
    source: 'AD7606', sample_rate_hz: 500, channel_count: 4, samples_written: 12345, dropped_frames: 2, recording_ms: 25000,
  };
  return deviceStatus({
    power,
    recording: fields,
    measurement: { available: true, state: 'RECORDING', object_id: OBJECT_A, session_id: 'S000007', samples_written: 12345, dropped_frames: 2, recording_ms: 25000 },
  });
}

function stubBrowser() {
  let nextId = 0;
  const timeouts = new Map<number, () => void>();
  const intervals = new Map<number, () => void>();
  const writes: string[] = [];
  let markReportWritten!: () => void;
  const reportWritten = new Promise<void>((resolve) => { markReportWritten = resolve; });
  const target = {
    document: {
      write: vi.fn((html: string) => { writes.push(html); }),
      open: vi.fn(),
      close: vi.fn(() => markReportWritten()),
    },
  };
  const browserWindow = {
    localStorage: { getItem: vi.fn(() => null), setItem: vi.fn() },
    setTimeout: (callback: () => void) => { const id = ++nextId; timeouts.set(id, callback); return id; },
    clearTimeout: (id: number) => { timeouts.delete(id); },
    setInterval: (callback: () => void) => { const id = ++nextId; intervals.set(id, callback); return id; },
    clearInterval: (id: number) => { intervals.delete(id); },
    confirm: vi.fn(() => false),
    open: vi.fn(() => target),
  };
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  vi.stubGlobal('window', browserWindow);
  return { timeouts, intervals, writes, target, reportWritten };
}

function fireNextTimeout(browser: ReturnType<typeof stubBrowser>): void {
  const next = browser.timeouts.entries().next().value as [number, () => void] | undefined;
  if (next === undefined) throw new Error('Expected a scheduled status poll.');
  browser.timeouts.delete(next[0]);
  next[1]();
}

function textOf(node: ReactTestInstance | string): string {
  return typeof node === 'string' ? node : node.children.map(textOf).join('');
}

function findButton(renderer: ReactTestRenderer, predicate: (button: ReactTestInstance) => boolean): ReactTestInstance {
  const button = renderer.root.findAllByType('button').find(predicate);
  if (button === undefined) throw new Error('Expected button was not rendered.');
  return button;
}

function expectLocaleLabels(renderer: ReactTestRenderer, locale: I18nLocale, keys: readonly I18nKey[]): string {
  const tree = JSON.stringify(renderer.toJSON());
  const other = locale === 'en' ? 'uk' : 'en';
  for (const key of keys) {
    const current = translate(locale, key);
    const opposite = translate(other, key);
    expect(tree).toContain(current);
    if (current !== opposite) expect(tree).not.toContain(opposite);
  }
  return tree;
}

function LocaleProbe({ expose }: { expose: (setLocale: (locale: I18nLocale) => void) => void }) {
  expose(useLocale().setLocale);
  return null;
}

async function mountApp(): Promise<{ renderer: ReactTestRenderer; setLocale: (locale: I18nLocale) => void }> {
  let setLocale!: (locale: I18nLocale) => void;
  let renderer!: ReactTestRenderer;
  await act(async () => {
    renderer = create(createElement(LocaleProvider, { initialLocale: 'en' },
      createElement(DeviceConsoleApp),
      createElement(LocaleProbe, { expose: (setter) => { setLocale = setter; } }),
    ));
    await Promise.resolve();
    await Promise.resolve();
  });
  mounted.push(renderer);
  return { renderer, setLocale: (locale) => setLocale(locale) };
}

const objectFeatureProps = {
  status: deviceStatus(),
  store: objectStore(),
  sessions: sessions(OBJECT_A, SESSION_A),
  selectedId: OBJECT_A,
  busy: false,
  error: null,
  onSelect: vi.fn(),
  onCreate: vi.fn(async () => true),
  onActivate: vi.fn(async () => undefined),
  onRename: vi.fn(async () => undefined),
  onDelete: vi.fn(async () => undefined),
  onBindGps: vi.fn(async () => ({ latitude: 0, longitude: 0, accuracy_m: 0, altitude_m: 0, altitude_accuracy_m: 0, captured_at_ms: 0, source: 'gps' })),
  onCable: vi.fn(async () => undefined),
  onDefaultCable: vi.fn(async () => undefined),
  onAcquisition: vi.fn(async () => undefined),
  onRecording: vi.fn(async () => undefined),
  onStartWorkflow: vi.fn(),
  onOpenScanner: vi.fn(),
};

beforeEach(() => {
  api.fetchDeviceStatus.mockReset().mockResolvedValue(deviceStatus());
  api.sendControl.mockReset().mockResolvedValue({ ok: true, action: 'noop' });
  api.fetchLocalizationRuns.mockReset().mockResolvedValue(null);
  api.fetchScanInfo.mockReset().mockResolvedValue(null);
  api.loadSessionRegistration.mockReset().mockResolvedValue(null);
  api.fetchSavedSeismicSession.mockReset().mockResolvedValue(null);
  api.loadObjectScanArtifact.mockReset().mockResolvedValue(null);
});

afterEach(() => {
  for (const renderer of mounted.splice(0)) act(() => renderer.unmount());
  vi.unstubAllGlobals();
  vi.clearAllMocks();
});

describe('locale changes during WebUI state transitions', () => {
  it('keeps an in-flight poll, recording state, and low-battery shutdown in one locale', async () => {
    const browser = stubBrowser();
    const gracePoll = deferred<DeviceStatus>();
    const shutdownPoll = deferred<DeviceStatus>();
    const polls = [gracePoll, shutdownPoll];
    api.fetchDeviceStatus.mockImplementation((includeObjects = false, sessionsObjectId?: string) => {
      if (includeObjects) return Promise.resolve(deviceStatus({ object_store: objectStore() }));
      if (sessionsObjectId !== undefined) return Promise.resolve(deviceStatus({ recording_sessions: sessions(sessionsObjectId, SESSION_A) }));
      return polls.shift()?.promise ?? Promise.resolve(deviceStatus());
    });

    const { renderer, setLocale } = await mountApp();
    const objectsTab = findButton(renderer, (button) => button.props['aria-pressed'] === false && textOf(button) === translate('en', 'nav.objects'));
    await act(async () => { objectsTab.props.onClick(); await Promise.resolve(); await Promise.resolve(); });
    fireNextTimeout(browser);
    expect(api.fetchDeviceStatus).toHaveBeenCalledWith(false, undefined, expect.objectContaining({ preemptible: true }));

    const requestsBeforeUk = api.fetchDeviceStatus.mock.calls.length;
    await act(async () => { setLocale('uk'); await loadUkrainianCatalog(); });
    expect(api.fetchDeviceStatus).toHaveBeenCalledTimes(requestsBeforeUk);
    await act(async () => { gracePoll.resolve(recordingUpdate(powerUpdate('grace', 7))); await gracePoll.promise; });

    const graceAlert = renderer.root.findAllByProps({ role: 'alert' }).find((node) => String(node.props.className).includes('power-banner'));
    expect(graceAlert).toBeDefined();
    expect(textOf(graceAlert!)).toBe(translate('uk', 'power.critical_grace', { seconds: formatNumber('uk', 7) }));
    const stopUk = findButton(renderer, (button) => textOf(button) === translate('uk', 'objects.stop_recording'));
    expect(stopUk.props.disabled).toBe(false);
    const ukrainianTree = expectLocaleLabels(renderer, 'uk', ['nav.objects', 'objects.recording', 'objects.stop_recording', 'objects.saved_measurements']);
    expect(ukrainianTree).toContain('S000007');
    expect(ukrainianTree).toContain(formatNumber('uk', 12345));
    expect(api.fetchDeviceStatus).toHaveBeenCalledTimes(requestsBeforeUk);

    fireNextTimeout(browser);
    const requestsBeforeEn = api.fetchDeviceStatus.mock.calls.length;
    await act(async () => { setLocale('en'); });
    expect(api.fetchDeviceStatus).toHaveBeenCalledTimes(requestsBeforeEn);
    await act(async () => { shutdownPoll.resolve(recordingUpdate(powerUpdate('shutdown'))); await shutdownPoll.promise; });

    const shutdownAlert = renderer.root.findAllByProps({ role: 'alert' }).find((node) => String(node.props.className).includes('power-banner'));
    expect(textOf(shutdownAlert!)).toBe(translate('en', 'power.critical_shutdown'));
    const englishTree = expectLocaleLabels(renderer, 'en', ['nav.objects', 'objects.recording', 'objects.stop_recording', 'objects.saved_measurements', 'power.critical_shutdown']);
    expect(englishTree).toContain('S000007');
    expect(englishTree).toContain(formatNumber('en', 12345));
    expect(api.sendControl).not.toHaveBeenCalled();
  });

  it('keeps the selected object session when an older session response resolves after the switch', async () => {
    stubBrowser();
    const sessionA = deferred<DeviceStatus>();
    const sessionB = deferred<DeviceStatus>();
    api.fetchDeviceStatus.mockImplementation((includeObjects = false, sessionsObjectId?: string) => {
      if (includeObjects) return Promise.resolve(deviceStatus({ object_store: objectStore() }));
      if (sessionsObjectId === OBJECT_A) return sessionA.promise;
      if (sessionsObjectId === OBJECT_B) return sessionB.promise;
      return Promise.resolve(deviceStatus());
    });
    const { renderer, setLocale } = await mountApp();

    const objectsTab = findButton(renderer, (button) => button.props['aria-pressed'] === false && textOf(button) === translate('en', 'nav.objects'));
    await act(async () => { objectsTab.props.onClick(); await Promise.resolve(); await Promise.resolve(); });
    expect(api.fetchDeviceStatus).toHaveBeenCalledWith(false, OBJECT_A, expect.objectContaining({ preemptible: true }));
    const objectBButton = findButton(renderer, (button) => String(button.props.className).includes('object-row') && textOf(button).includes(OBJECT_B));
    await act(async () => { objectBButton.props.onClick(); await Promise.resolve(); await Promise.resolve(); });
    expect(api.fetchDeviceStatus).toHaveBeenCalledWith(false, OBJECT_B, expect.objectContaining({ preemptible: true }));

    const requestsBeforeUk = api.fetchDeviceStatus.mock.calls.length;
    await act(async () => { setLocale('uk'); await loadUkrainianCatalog(); });
    expect(api.fetchDeviceStatus).toHaveBeenCalledTimes(requestsBeforeUk);

    await act(async () => {
      sessionB.resolve(deviceStatus({ power: powerUpdate('grace', 9), recording_sessions: sessions(OBJECT_B, SESSION_B) }));
      await sessionB.promise;
    });
    expect(JSON.stringify(renderer.toJSON())).toContain(SESSION_B);
    await act(async () => {
      sessionA.resolve(deviceStatus({ power: powerUpdate('shutdown'), recording_sessions: sessions(OBJECT_A, SESSION_A) }));
      await sessionA.promise;
    });

    const tree = expectLocaleLabels(renderer, 'uk', ['nav.objects', 'objects.sessions', 'objects.saved_measurements']);
    expect(tree).toContain('Stone arch');
    expect(tree).toContain(SESSION_B);
    expect(tree).toContain(formatNumber('uk', 12000));
    expect(tree).not.toContain(SESSION_A);
    const alert = renderer.root.findAllByProps({ role: 'alert' }).find((node) => String(node.props.className).includes('power-banner'));
    expect(textOf(alert!)).toBe(translate('uk', 'power.critical_grace', { seconds: formatNumber('uk', 9) }));
    expect(tree).not.toContain(translate('uk', 'power.critical_shutdown'));
    expect(api.fetchDeviceStatus).toHaveBeenCalledTimes(requestsBeforeUk);
  });

  it('keeps a session view and PDF export in the locale selected when export starts', async () => {
    const browser = stubBrowser();
    await import('../../src/features/seismic/SavedSeismicViewer');
    const evidenceStarted = deferred<void>();
    const exportStarted = deferred<void>();
    const exportRuns = deferred<null>();
    let runsCalls = 0;
    api.fetchLocalizationRuns.mockImplementation(() => {
      runsCalls += 1;
      if (runsCalls === 1) { evidenceStarted.resolve(); return Promise.resolve(null); }
      if (runsCalls === 2) { exportStarted.resolve(); return exportRuns.promise; }
      return Promise.resolve(null);
    });

    let setLocale!: (locale: I18nLocale) => void;
    let renderer!: ReactTestRenderer;
    function ObjectsProbe() {
      setLocale = useLocale().setLocale;
      const [status] = useState(deviceStatus());
      return createElement(Fragment, null,
        createElement(PowerBanner, { status }),
        createElement(ObjectsFeature, objectFeatureProps),
      );
    }
    await act(async () => {
      renderer = create(createElement(LocaleProvider, { initialLocale: 'en' }, createElement(ObjectsProbe)));
    });
    mounted.push(renderer);

    const sessionRow = findButton(renderer, (button) => button.props.className === 'session-row' && textOf(button).includes(SESSION_A));
    await act(async () => { sessionRow.props.onClick(); await Promise.resolve(); });
    await evidenceStarted.promise;
    await act(async () => { await new Promise<void>((resolve) => globalThis.setTimeout(resolve, 0)); });
    expect(api.fetchSavedSeismicSession).toHaveBeenCalledTimes(1);
    const exportButton = findButton(renderer, (button) => textOf(button) === translate('en', 'objects.export_pdf'));
    act(() => { exportButton.props.onClick(); });
    await exportStarted.promise;
    expect(browser.target.document.write).toHaveBeenCalledTimes(1);

    const requestsBeforeUk = {
      runs: api.fetchLocalizationRuns.mock.calls.length,
      evidenceScans: api.fetchScanInfo.mock.calls.length,
      waveform: api.fetchSavedSeismicSession.mock.calls.length,
      reportScan: api.loadObjectScanArtifact.mock.calls.length,
    };
    await act(async () => { setLocale('uk'); await loadUkrainianCatalog(); });
    expectLocaleLabels(renderer, 'uk', ['objects.export_pdf', 'objects.scan', 'seismic.saved_heading', 'evidence.title']);
    expect(api.fetchLocalizationRuns).toHaveBeenCalledTimes(requestsBeforeUk.runs);
    expect(api.fetchScanInfo).toHaveBeenCalledTimes(requestsBeforeUk.evidenceScans);
    expect(api.fetchSavedSeismicSession).toHaveBeenCalledTimes(requestsBeforeUk.waveform);
    expect(api.loadObjectScanArtifact).toHaveBeenCalledTimes(requestsBeforeUk.reportScan);

    await act(async () => { exportRuns.resolve(null); await exportRuns.promise; await browser.reportWritten; });
    const report = browser.writes.at(-1)!;
    for (const key of ['report.coordinate_frames', 'report.location', 'report.recording', 'report.print_pdf'] as const) {
      expect(report).toContain(translate('en', key));
      expect(report).not.toContain(translate('uk', key));
    }
    expect(report).toContain('Bridge pier');
    expect(report).toContain(SESSION_A);
    expect(report).toContain(formatNumber('en', 12000));
    expect(api.loadObjectScanArtifact).toHaveBeenCalledTimes(1);
    expect(api.fetchLocalizationRuns).toHaveBeenCalledTimes(requestsBeforeUk.runs);
  });
});
