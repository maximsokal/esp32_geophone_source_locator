import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createElement } from 'react';
import { act, create, type ReactTestRenderer } from 'react-test-renderer';

const h = vi.hoisted(() => ({
  props: null as any,
  viewportProps: null as any,
  decodeWorkers: [] as any[],
  fetchRuns: vi.fn(), loadScan: vi.fn(), loadReg: vi.fn(), saveReg: vi.fn(), delReg: vi.fn(),
  saveScan: vi.fn(), saveOcc: vi.fn(), savePrev: vi.fn(), info: vi.fn(), delScan: vi.fn(),
}));

vi.mock('../../src/api/deviceApi', async (importOriginal) => {
  const actual = await importOriginal<typeof import('../../src/api/deviceApi')>();
  return {
    ...actual,
    fetchLocalizationRuns: h.fetchRuns,
    loadObjectScanArtifact: h.loadScan,
    loadSessionRegistration: h.loadReg,
    saveSessionRegistration: h.saveReg,
    deleteSessionRegistration: h.delReg,
    saveObjectScanArtifact: h.saveScan,
    saveSessionOccupancyArtifact: h.saveOcc,
    saveSessionPreviewArtifact: h.savePrev,
    fetchScanInfo: h.info,
    deleteObjectScanArtifact: h.delScan,
  };
});

vi.mock('../../src/components/XrRuntimePanel', () => ({ XrRuntimePanel: (props: unknown) => { h.props = props; return null; } }));
vi.mock('../../src/components/ScanPointCloudViewport', () => ({ ScanPointCloudViewport: (props: unknown) => { h.viewportProps = props; return null; } }));
vi.mock('../../src/xr/deviceComputeProfile', () => ({ deviceComputeProfilePromise: new Promise(() => undefined) }));
vi.mock('../../src/xr/scanDecodeWorker?worker&inline', () => ({
  default: class {
    listeners = new Map<string, (event: any) => void>();
    request: any = null;
    transfer: Transferable[] = [];
    terminated = false;
    constructor() { h.decodeWorkers.push(this); }
    addEventListener(type: string, listener: (event: any) => void): void { this.listeners.set(type, listener); }
    removeEventListener(type: string): void { this.listeners.delete(type); }
    postMessage(request: any, transfer: Transferable[] = []): void {
      this.transfer = transfer;
      this.request = structuredClone(request, { transfer });
    }
    terminate(): void { this.terminated = true; }
    send(data: any): void { this.listeners.get('message')?.({ data }); }
    fail(message: string): void { this.listeners.get('error')?.({ message }); }
  },
}));
vi.mock('../../src/xr/scanEncodeWorker?worker&inline', () => ({ default: class {} }));

import App from '../../src/App';
import { LocaleProvider } from '../../src/i18n';
import * as scanArtifact from '../../src/xr/scanArtifact';
import { encodeScanArtifact } from '../../src/xr/scanArtifact';
import { processScanDecodeWorkerMessage, type ScanDecodeWorkerResponse } from '../../src/xr/scanDecodeWorker';
import type { ScanPointCloudSnapshot } from '../../src/xr/webxrRuntime';
import { makeValidScanSnapshot } from '../xr/support/validScanSnapshot';

const mounted: ReactTestRenderer[] = [];
const device = { scan: null as Uint8Array | null };

async function flush(): Promise<void> {
  await act(async () => { for (let index = 0; index < 200; index += 1) await Promise.resolve(); });
}

async function mount(objectId = 'OBJ', sessionId = 'S'): Promise<ReactTestRenderer> {
  let renderer!: ReactTestRenderer;
  await act(async () => {
    renderer = create(createElement(LocaleProvider, { initialLocale: 'en' }, createElement(App, { objectId, sessionId, status: null })));
  });
  mounted.push(renderer);
  await flush();
  return renderer;
}

function workerResult(worker: any): ScanDecodeWorkerResponse {
  let response: ScanDecodeWorkerResponse | null = null;
  processScanDecodeWorkerMessage(worker.request, { postMessage: (message) => { response = message; } });
  if (response === null) throw new Error('Scan decode worker produced no response.');
  return response;
}

function startFreshScan(): ScanPointCloudSnapshot {
  const snapshot = makeValidScanSnapshot();
  h.props.onPointCloudReady(null);
  h.props.onPointCloudReady(snapshot);
  return snapshot;
}

beforeEach(() => {
  h.props = null; h.viewportProps = null; h.decodeWorkers = [];
  device.scan = null;
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  vi.stubGlobal('Worker', class {});
  vi.stubGlobal('window', {
    localStorage: { getItem: () => null, setItem: () => undefined },
    addEventListener: () => undefined, removeEventListener: () => undefined, isSecureContext: false,
    location: { href: 'http://localhost/' }, setTimeout: globalThis.setTimeout.bind(globalThis),
    clearTimeout: globalThis.clearTimeout.bind(globalThis),
  });
  vi.stubGlobal('document', { hidden: false, visibilityState: 'visible', addEventListener: () => undefined, removeEventListener: () => undefined });
  vi.stubGlobal('navigator', {});
  h.fetchRuns.mockReset().mockResolvedValue({ object_id: 'OBJ', session_id: 'S', runs: [], geometry: { available: true, pods: [] } });
  h.loadScan.mockReset().mockImplementation(async () => device.scan?.slice() ?? null);
  h.loadReg.mockReset().mockResolvedValue(null);
  h.saveReg.mockReset().mockResolvedValue({ ok: true, action: 'registration' });
  h.delReg.mockReset().mockResolvedValue({ ok: true, action: 'delete' });
  h.delScan.mockReset().mockResolvedValue({ ok: true, action: 'delete' });
  h.saveScan.mockReset();
  h.saveOcc.mockReset().mockResolvedValue({ ok: true, action: 'occupancy' });
  h.savePrev.mockReset().mockResolvedValue({ ok: true, action: 'preview' });
  h.info.mockReset().mockImplementation(async (_object: string, _session: string, kind: 'scan' | 'occupancy' | 'preview') => {
    if (device.scan === null) return { present: false, bytes: 0 };
    return kind === 'scan'
      ? { present: true, bytes: device.scan.byteLength, id: null }
      : { present: false, bytes: 0, sourceId: null };
  });
});

afterEach(() => {
  for (const renderer of [...mounted]) { act(() => renderer.unmount()); mounted.splice(mounted.indexOf(renderer), 1); }
  vi.unstubAllGlobals();
});

describe('App off-thread saved scan decode lifecycle (MAX-392)', () => {
  it('shows the reading status while worker validation is pending and does not expose an unvalidated snapshot', async () => {
    const bytes = encodeScanArtifact(makeValidScanSnapshot());
    device.scan = bytes.slice();
    const syncDecoder = vi.spyOn(scanArtifact, 'decodeScanArtifactWithIdentityAndMetrics');
    const renderer = await mount();
    const worker = h.decodeWorkers[0];

    expect(h.decodeWorkers).toHaveLength(1);
    expect(worker.request.type).toBe('decode');
    expect(worker.request.artifact.byteLength).toBe(bytes.byteLength);
    expect(worker.transfer).toHaveLength(1);
    expect(worker.transfer[0]).not.toBe(worker.request.artifact);
    expect((worker.transfer[0] as ArrayBuffer).byteLength).toBe(0);
    expect(syncDecoder).not.toHaveBeenCalled();
    expect(device.scan).toEqual(bytes);
    expect(JSON.stringify(renderer.toJSON())).toContain('Reading saved scan…');
    expect(h.info).not.toHaveBeenCalled();
    expect(h.saveOcc).not.toHaveBeenCalled();
    expect(h.savePrev).not.toHaveBeenCalled();
    expect(h.viewportProps).toBeNull();
    syncDecoder.mockRestore();
  });

  it('publishes and reconciles the validated snapshot only after the worker response', async () => {
    device.scan = encodeScanArtifact(makeValidScanSnapshot());
    await mount();
    const worker = h.decodeWorkers[0];

    await act(async () => { worker.send(workerResult(worker)); });
    await flush();

    expect(worker.terminated).toBe(true);
    expect(h.viewportProps.snapshot.pointCount).toBe(1);
    expect(h.info).toHaveBeenCalledTimes(3);
    expect(h.saveScan).not.toHaveBeenCalled();
  });

  it('keeps the known-good device scan untouched when the decode worker crashes', async () => {
    const knownGood = encodeScanArtifact(makeValidScanSnapshot());
    device.scan = knownGood.slice();
    const renderer = await mount();
    const worker = h.decodeWorkers[0];

    await act(async () => { worker.fail('worker crashed'); });
    await flush();

    expect(worker.terminated).toBe(true);
    expect(device.scan).toEqual(knownGood);
    expect(h.info).not.toHaveBeenCalled();
    expect(h.saveScan).not.toHaveBeenCalled();
    expect(h.saveOcc).not.toHaveBeenCalled();
    expect(h.savePrev).not.toHaveBeenCalled();
    expect(JSON.stringify(renderer.toJSON())).toContain('Could not restore the saved 3D scan.');
  });

  it('terminates on object switch and drops a late result from the previous owner', async () => {
    device.scan = encodeScanArtifact(makeValidScanSnapshot());
    const renderer = await mount();
    const previous = h.decodeWorkers[0];
    const previousHandler = previous.listeners.get('message');
    const stale = workerResult(previous);
    const staleBuffer = stale.type === 'result' ? stale.snapshot.positions.buffer as ArrayBuffer : null;

    await act(async () => {
      renderer.update(createElement(LocaleProvider, { initialLocale: 'en' }, createElement(App, { objectId: 'OBJ-2', sessionId: 'S-2', status: null })));
    });
    await flush();
    previousHandler?.({ data: stale } as MessageEvent<ScanDecodeWorkerResponse>);
    await flush();

    expect(previous.terminated).toBe(true);
    expect(h.decodeWorkers).toHaveLength(2);
    expect(h.info).not.toHaveBeenCalled();
    expect(h.saveOcc).not.toHaveBeenCalled();
    expect(staleBuffer?.byteLength).toBe(0);
  });

  it.each(['scanner leave', 'Fresh', 'Delete'] as const)(
    'terminates and rejects a late result after %s', async (action) => {
      const bytes = encodeScanArtifact(makeValidScanSnapshot());
      device.scan = bytes.slice();
      await mount();
      const worker = h.decodeWorkers[0];
      const lateHandler = worker.listeners.get('message');
      const stale = workerResult(worker);
      const staleBuffer = stale.type === 'result' ? stale.snapshot.positions.buffer as ArrayBuffer : null;

      if (action === 'scanner leave') await act(async () => { h.props.onSessionActiveChange(false); });
      else if (action === 'Fresh') await act(async () => { expect(h.props.onScanIntent('fresh')).toBe(true); });
      else await act(async () => { h.props.onDeleteSavedScan(); });
      await flush();
      lateHandler?.({ data: stale } as MessageEvent<ScanDecodeWorkerResponse>);
      await flush();

      expect(worker.terminated).toBe(true);
      expect(staleBuffer?.byteLength).toBe(0);
      expect(device.scan).toEqual(bytes);
      expect(h.saveScan).not.toHaveBeenCalled();
      expect(h.saveOcc).not.toHaveBeenCalled();
      expect(h.savePrev).not.toHaveBeenCalled();
      if (action === 'Delete') expect(h.delScan).toHaveBeenCalledTimes(1);
    },
  );
});
