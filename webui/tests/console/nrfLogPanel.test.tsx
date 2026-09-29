import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createElement } from 'react';
import { act, create, type ReactTestInstance, type ReactTestRenderer } from 'react-test-renderer';

const api = vi.hoisted(() => ({ fetchNrfLogs: vi.fn() }));
vi.mock('../../src/api/deviceApi', async (importOriginal) => {
  const actual = await importOriginal<typeof import('../../src/api/deviceApi')>();
  return { ...actual, ...api };
});

import NrfLogPanel, { NRF_LOG_PANEL_I18N_KEYS, nrfLogsCsv, nrfLogsJson } from '../../src/features/nrflog/NrfLogPanel';
import { NRF_LOG_EVENTS } from '../../src/features/nrflog/nrfLogEvents.gen';
import type { DeviceStatus, NrfLogNodeSummary, NrfLogPage, NrfLogRecord } from '../../src/domain/device';
import { en } from '../../src/i18n/catalog.en.gen';
import { uk } from '../../src/i18n/catalog.uk.gen';
import type { I18nKey } from '../../src/i18n';

const sample = (overrides: Partial<NrfLogRecord> = {}): NrfLogRecord => ({
  token: 'opaque-token-1', boot_id: 11, seq: 1, uptime_ms: 100, esp_rx_uptime_ms: 120,
  event_id: 1, severity: 1, subsystem: 1, arg0: 0, arg1: 0, ...overrides,
});

const summary = (overrides: Partial<NrfLogNodeSummary> = {}): NrfLogNodeSummary => ({
  node: 'remote2', channel: 2, present: true, logs_unsupported: false, boot_id: 11,
  stored: 1, warn_count: 0, error_count: 0, fatal_count: 0, wire_gap_total: 0,
  local_drop_total: 0, overwritten_total: 0, ...overrides,
});

const status = (...nodes: NrfLogNodeSummary[]): DeviceStatus => ({ nrf_logs: nodes } as unknown as DeviceStatus);

const okPage = (records: NrfLogRecord[], overrides: Partial<NrfLogPage> = {}) => ({
  ok: true as const,
  value: {
    ok: true as const,
    node: 'remote2' as const,
    channel: 2,
    records,
    next_token: records.at(-1)?.token ?? null,
    reset: false,
    counters: { overwritten_total: 10, lost_internal_total: 20, wire_gap_total: 30, local_drop_total: 40 },
    logs_unsupported: false,
    boot_id: 11,
    ...overrides,
  },
});

function text(node: ReactTestInstance | string | number): string {
  return typeof node === 'string' || typeof node === 'number'
    ? String(node)
    : node.children.map((child) => text(child as ReactTestInstance | string | number)).join(' ');
}

let renderer: ReactTestRenderer | undefined;

async function mount(nodeStatus: DeviceStatus = status(summary())): Promise<ReactTestRenderer> {
  await act(async () => {
    renderer = create(createElement(NrfLogPanel, { status: nodeStatus }));
  });
  return renderer!;
}

beforeEach(() => {
  vi.useFakeTimers();
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  vi.stubGlobal('window', {
    setTimeout: (callback: () => void, delay: number) => globalThis.setTimeout(callback, delay),
    clearTimeout: (id: number) => globalThis.clearTimeout(id),
  });
  api.fetchNrfLogs.mockReset().mockResolvedValue(okPage([]));
});

afterEach(() => {
  if (renderer !== undefined) act(() => renderer?.unmount());
  renderer = undefined;
  vi.useRealTimers();
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe('nRF log panel', () => {
  it('pages forward with the opaque next token and replaces records on reset', async () => {
    const first = sample({ token: 'opaque/first', event_id: 1 });
    const second = sample({ token: 'opaque/second', seq: 2, event_id: 6 });
    const resetRecord = sample({ token: 'opaque/new-boot', boot_id: 12, seq: 1, event_id: 7 });
    api.fetchNrfLogs
      .mockResolvedValueOnce(okPage([first], { next_token: first.token }))
      .mockResolvedValueOnce(okPage([second], { next_token: second.token }))
      .mockResolvedValueOnce(okPage([resetRecord], { next_token: resetRecord.token, reset: true, boot_id: 12 }));

    const view = await mount();
    expect(text(view.root)).toContain('Device booted');
    expect(api.fetchNrfLogs).toHaveBeenNthCalledWith(1, 'remote2', undefined, 200);

    await act(async () => { await vi.advanceTimersByTimeAsync(2000); });
    expect(api.fetchNrfLogs).toHaveBeenNthCalledWith(2, 'remote2', first.token, 200);
    expect(text(view.root)).toContain('Network role assigned');

    await act(async () => { await vi.advanceTimersByTimeAsync(2000); });
    expect(api.fetchNrfLogs).toHaveBeenNthCalledWith(3, 'remote2', second.token, 200);
    expect(text(view.root)).toContain('Log history changed; reloaded the oldest available records.');
    expect(text(view.root)).toContain('Network role lost');
    expect(text(view.root)).not.toContain('Device booted');
  });

  it('filters by inclusive minimum severity', async () => {
    api.fetchNrfLogs.mockResolvedValue(okPage([
      sample({ token: 'info', event_id: 1, severity: 1 }),
      sample({ token: 'error', event_id: 8, severity: 3 }),
    ]));
    const view = await mount();
    expect(text(view.root)).toContain('Device booted');
    expect(text(view.root)).toContain('IMU fault detected');

    const filter = view.root.findByProps({ 'aria-label': 'Minimum log severity' });
    act(() => filter.props.onChange({ target: { value: '3' } }));
    expect(text(view.root)).not.toContain('Device booted');
    expect(text(view.root)).toContain('IMU fault detected');
  });

  it('keeps unknown event and subsystem values and raw arguments visible', async () => {
    api.fetchNrfLogs.mockResolvedValue(okPage([sample({ event_id: 65000, subsystem: 255, arg0: 1234, arg1: 5678 })]));
    const view = await mount();
    expect(text(view.root)).toContain('Unknown event #65000');
    expect(text(view.root)).toContain('#255');
    expect(text(view.root)).toContain('arg0: Unknown value (1234), arg1: Unknown value (5678)');
  });

  it('renders decoded event arguments and exposes unsigned raw args in the tooltip', async () => {
    api.fetchNrfLogs.mockResolvedValue(okPage([sample({ event_id: 3, arg0: 0x80, arg1: 6 })]));
    const view = await mount();
    expect(text(view.root)).toContain('Message type: 0x00000080, Decoder error: Bad CRC (6)');
    const argsCell = view.root.findAllByType('td').find((cell) => typeof cell.props.title === 'string');
    expect(argsCell?.props.title).toBe('Raw arg0: 128; arg1: 6');
  });

  it('renders the four loss counters independently', async () => {
    api.fetchNrfLogs.mockResolvedValue(okPage([], {
      counters: { overwritten_total: 101, lost_internal_total: 202, wire_gap_total: 303, local_drop_total: 404 },
    }));
    const view = await mount();
    expect(view.root.findAllByType('dd').map(text)).toEqual(['101', '202', '303', '404']);
  });

  it('shows the unsupported-firmware message and stops polling', async () => {
    api.fetchNrfLogs.mockResolvedValue(okPage([], { logs_unsupported: true }));
    const view = await mount();
    expect(text(view.root)).toContain("This node's firmware does not support logs");
    await act(async () => { await vi.advanceTimersByTimeAsync(5000); });
    expect(api.fetchNrfLogs).toHaveBeenCalledTimes(1);
  });

  it('inserts a reboot separator when the page boot id changes', async () => {
    api.fetchNrfLogs
      .mockResolvedValueOnce(okPage([sample({ token: 'before', boot_id: 11 })], { boot_id: 11 }))
      .mockResolvedValueOnce(okPage([sample({ token: 'after', boot_id: 12, seq: 1 })], { boot_id: 12 }));
    const view = await mount();
    await act(async () => { await vi.advanceTimersByTimeAsync(2000); });
    expect(view.root.findAllByProps({ className: 'nrf-log-reboot' })).toHaveLength(1);
    expect(text(view.root)).toContain('Node rebooted');
  });

  it('exports the currently loaded records as JSON and escaped CSV', async () => {
    const record = sample({ token: 'token,"one"' });
    api.fetchNrfLogs.mockResolvedValue(okPage([record]));
    const blobs: Blob[] = [];
    const anchors: Array<{ href: string; download: string; click: ReturnType<typeof vi.fn>; remove: ReturnType<typeof vi.fn> }> = [];
    vi.stubGlobal('URL', {
      createObjectURL: vi.fn((blob: Blob) => { blobs.push(blob); return `blob:${blobs.length}`; }),
      revokeObjectURL: vi.fn(),
    });
    vi.stubGlobal('document', {
      visibilityState: 'visible',
      addEventListener: vi.fn(),
      removeEventListener: vi.fn(),
      body: { appendChild: vi.fn() },
      createElement: vi.fn(() => {
        const anchor = { href: '', download: '', click: vi.fn(), remove: vi.fn() };
        anchors.push(anchor);
        return anchor;
      }),
    });
    const view = await mount();
    const buttons = view.root.findAllByType('button');

    act(() => buttons.find((button) => text(button) === 'Export JSON')?.props.onClick());
    expect(anchors[0].download).toBe('nrf-logs-remote2.json');
    expect(JSON.parse(await blobs[0].text())).toEqual([record]);

    act(() => buttons.find((button) => text(button) === 'Export CSV')?.props.onClick());
    expect(anchors[1].download).toBe('nrf-logs-remote2.csv');
    const csv = await blobs[1].text();
    expect(csv.split('\r\n')[0]).toBe('"token","boot_id","seq","uptime_ms","esp_rx_uptime_ms","event_id","severity","subsystem","arg0","arg1","arguments_decoded"');
    expect(csv).toContain('"Reset cause bits (0 = unknown): 0x00000000, Boot ID fallback: Not used (0)"');
    expect(csv).toContain('"token,""one"""');
    expect(anchors.every((anchor) => anchor.click.mock.calls.length === 1 && anchor.remove.mock.calls.length === 1)).toBe(true);
    expect(nrfLogsJson([record])).toBe(JSON.stringify([record], null, 2));
    expect(nrfLogsCsv([record], (key) => en[key])).toBe(csv);
  });

  it('pauses polling while the document is hidden and resumes when visible', async () => {
    let visibilityChange: (() => void) | undefined;
    const documentStub = {
      visibilityState: 'visible',
      addEventListener: (_event: string, listener: () => void) => { visibilityChange = listener; },
      removeEventListener: vi.fn(),
    };
    vi.stubGlobal('document', documentStub);
    const view = await mount();
    expect(api.fetchNrfLogs).toHaveBeenCalledTimes(1);

    documentStub.visibilityState = 'hidden';
    act(() => visibilityChange?.());
    await act(async () => { await vi.advanceTimersByTimeAsync(5000); });
    expect(api.fetchNrfLogs).toHaveBeenCalledTimes(1);

    documentStub.visibilityState = 'visible';
    await act(async () => { visibilityChange?.(); });
    expect(api.fetchNrfLogs).toHaveBeenCalledTimes(2);
    await act(async () => { await vi.advanceTimersByTimeAsync(2000); });
    expect(api.fetchNrfLogs).toHaveBeenCalledTimes(3);
    act(() => view.unmount());
    renderer = undefined;
  });

  it('includes every nrf_log key used by the panel and generated event lookup in EN and UK', () => {
    const eventKeys = Object.values(NRF_LOG_EVENTS).map((event) => `nrf_log.event.${event.name.toLowerCase()}` as I18nKey);
    for (const key of [...NRF_LOG_PANEL_I18N_KEYS, ...eventKeys]) {
      expect(en[key], `${key} English`).toBeTruthy();
      expect(uk[key], `${key} Ukrainian`).toBeTruthy();
    }
  });

  it('shows only present nodes and nodes with stored records', async () => {
    api.fetchNrfLogs.mockResolvedValue(okPage([]));
    const view = await mount(status(
      summary({ node: 'main', channel: null, present: false, stored: 0 }),
      summary({ node: 'remote0', channel: 0, present: false, stored: 2 }),
      summary({ node: 'remote2', channel: 2, present: true, stored: 0 }),
    ));
    const selector = view.root.findByProps({ 'aria-label': 'Select nRF log node' });
    expect(selector.findAllByType('option').map(text)).toEqual(['Pod 1', 'Pod 3']);
  });
});
