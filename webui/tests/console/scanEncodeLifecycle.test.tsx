import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createElement } from 'react';
import { act, create, type ReactTestRenderer } from 'react-test-renderer';

const h = vi.hoisted(() => ({
  props: null as any,
  viewportProps: null as any,
  encodeWorkers: [] as any[],
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
vi.mock('../../src/xr/scanEncodeWorker?worker&inline', () => ({
  default: class {
    listeners = new Map<string, (event: any) => void>();
    request: any = null;
    transfer: Transferable[] = [];
    terminated = false;
    constructor() { h.encodeWorkers.push(this); }
    addEventListener(type: string, listener: (event: any) => void): void { this.listeners.set(type, listener); }
    removeEventListener(type: string): void { this.listeners.delete(type); }
    postMessage(request: any, transfer: Transferable[] = []): void { this.request = request; this.transfer = transfer; }
    terminate(): void { this.terminated = true; }
    send(data: any): void { this.listeners.get('message')?.({ data }); }
    fail(message: string): void { this.listeners.get('error')?.({ message }); }
  },
}));

import App from '../../src/App';
import { LocaleProvider } from '../../src/i18n';
import { encodeScanArtifact, encodeScanArtifactWithIdentity } from '../../src/xr/scanArtifact';
import { occupancyFrameIdForScan } from '../../src/xr/occupancySolverArtifact';
import { processScanEncodeWorkerMessage, type ScanEncodeWorkerResponse } from '../../src/xr/scanEncodeWorker';
import type { ScanPointCloudSnapshot } from '../../src/xr/webxrRuntime';
import { makeValidScanSnapshot } from '../xr/support/validScanSnapshot';

const mounted: ReactTestRenderer[] = [];
const device = { scan: null as Uint8Array | null, occupancy: null as Uint8Array | null, preview: null as Uint8Array | null };

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

function workerResult(worker: any): ScanEncodeWorkerResponse {
  let response: ScanEncodeWorkerResponse | null = null;
  processScanEncodeWorkerMessage(worker.request, { postMessage: (message) => { response = message; } });
  if (response === null) throw new Error('Scan encode worker produced no response.');
  return response;
}

function startFinish(snapshot: ScanPointCloudSnapshot = makeValidScanSnapshot()): ScanPointCloudSnapshot {
  h.props.onPointCloudReady(null);
  h.props.onPointCloudReady(snapshot);
  return snapshot;
}

beforeEach(() => {
  h.props = null; h.viewportProps = null; h.encodeWorkers = [];
  device.scan = null; device.occupancy = null; device.preview = null;
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
  h.loadScan.mockReset().mockImplementation(async () => device.scan);
  h.loadReg.mockReset().mockResolvedValue(null);
  h.saveReg.mockReset().mockResolvedValue({ ok: true, action: 'registration' });
  h.delReg.mockReset().mockResolvedValue({ ok: true, action: 'delete' });
  h.delScan.mockReset().mockResolvedValue({ ok: true, action: 'delete' });
  h.saveScan.mockReset().mockImplementation(async (_object: string, _session: string, bytes: Uint8Array) => {
    device.scan = bytes.slice(); device.occupancy = null; device.preview = null;
    return { ok: true, action: 'scan', id: occupancyFrameIdForScan(bytes) };
  });
  h.saveOcc.mockReset().mockImplementation(async (_object: string, _session: string, bytes: Uint8Array) => { device.occupancy = bytes.slice(); return { ok: true, action: 'occupancy' }; });
  h.savePrev.mockReset().mockImplementation(async (_object: string, _session: string, bytes: Uint8Array) => { device.preview = bytes.slice(); return { ok: true, action: 'preview' }; });
  h.info.mockReset().mockImplementation(async (_object: string, _session: string, kind: 'scan' | 'occupancy' | 'preview') => {
    const bytes = kind === 'scan' ? device.scan : kind === 'occupancy' ? device.occupancy : device.preview;
    if (bytes === null) return { present: false, bytes: 0 };
    const currentId = device.scan === null ? null : occupancyFrameIdForScan(device.scan);
    return kind === 'scan'
      ? { present: true, bytes: bytes.byteLength, id: currentId }
      : { present: true, bytes: bytes.byteLength, id: null, sourceId: currentId };
  });
});

afterEach(() => {
  for (const renderer of [...mounted]) { act(() => renderer.unmount()); mounted.splice(mounted.indexOf(renderer), 1); }
  vi.unstubAllGlobals();
});

describe('App off-thread scan encoding lifecycle (MAX-392)', () => {
  it('returns while encoding is pending, shows the preparing status, and does not call the sync encoder or persistence', async () => {
    const renderer = await mount();
    const syncEncoder = vi.spyOn(await import('../../src/xr/scanArtifact'), 'encodeScanArtifactWithIdentity');
    const snapshot = makeValidScanSnapshot();

    await act(async () => { startFinish(snapshot); h.props.onSessionActiveChange(true); });
    await flush();

    expect(h.encodeWorkers).toHaveLength(1);
    expect(h.encodeWorkers[0].request.type).toBe('encode');
    expect(h.encodeWorkers[0].request.snapshot.positions).not.toBe(snapshot.positions);
    expect(h.encodeWorkers[0].transfer).toContain(h.encodeWorkers[0].request.snapshot.positions.buffer);
    expect(h.saveScan).not.toHaveBeenCalled();
    expect(syncEncoder).not.toHaveBeenCalled();
    expect(JSON.stringify(renderer.toJSON())).toContain('Preparing scan for saving · OBJ');
  });

  it('keeps the durable base and enters a clean encode error when the worker crashes', async () => {
    const baseBytes = encodeScanArtifact(makeValidScanSnapshot());
    device.scan = baseBytes.slice();
    const renderer = await mount();
    const baseSnapshot = h.viewportProps.snapshot as ScanPointCloudSnapshot;
    await act(async () => { startFinish(); });
    await flush();
    const worker = h.encodeWorkers[0];

    await act(async () => { worker.fail('worker crashed'); });
    await flush();

    expect(worker.terminated).toBe(true);
    expect(h.saveScan).not.toHaveBeenCalled();
    expect(h.saveOcc).not.toHaveBeenCalled();
    expect(h.savePrev).not.toHaveBeenCalled();
    expect(device.scan).toEqual(baseBytes);
    expect(h.viewportProps.snapshot).toBe(baseSnapshot);
    const rendered = JSON.stringify(renderer.toJSON());
    expect(rendered).toContain('Could not prepare the 3D scan for saving.');
    expect(rendered).toContain('3D storage error · OBJ');
  });

  it('uses the worker artifact and identity before starting persistence', async () => {
    const renderer = await mount();
    await act(async () => { startFinish(); h.props.onTagRegistrationResult(null, true); });
    const worker = h.encodeWorkers[0];
    const response = workerResult(worker);
    expect(response.type).toBe('result');

    await act(async () => { worker.send(response); });
    await flush();

    expect(worker.terminated).toBe(true);
    expect(h.saveScan).toHaveBeenCalledTimes(1);
    expect(h.saveOcc).toHaveBeenCalledTimes(1);
    expect(h.savePrev).toHaveBeenCalledTimes(1);
    expect(device.scan).not.toBeNull();
    expect(JSON.stringify(renderer.toJSON())).toContain('3D scan saved to SD · OBJ');
  });

  it.each(['newer Finish', 'scanner leave', 'Fresh', 'Delete', 'object switch', 'session switch'] as const)(
    'terminates the in-flight worker and rejects a late result after %s', async (action) => {
      const renderer = await mount();
      await act(async () => { startFinish(); });
      const worker = h.encodeWorkers[0];
      const oldHandler = worker.listeners.get('message');
      const request = worker.request;
      const stale = encodeScanArtifactWithIdentity(makeValidScanSnapshot());

      if (action === 'newer Finish') {
        await act(async () => { h.props.onPointCloudReady(makeValidScanSnapshot()); });
      } else if (action === 'scanner leave') {
        await act(async () => { h.props.onSessionActiveChange(false); });
      } else if (action === 'Fresh') {
        await act(async () => { expect(h.props.onScanIntent('fresh')).toBe(true); });
      } else if (action === 'Delete') {
        await act(async () => { h.props.onDeleteSavedScan(); });
      } else if (action === 'object switch') {
        await act(async () => {
          renderer.update(createElement(LocaleProvider, { initialLocale: 'en' }, createElement(App, { objectId: 'OBJ-2', sessionId: 'S-2', status: null })));
        });
      } else {
        await act(async () => {
          renderer.update(createElement(LocaleProvider, { initialLocale: 'en' }, createElement(App, { objectId: 'OBJ', sessionId: 'S-2', status: null })));
        });
      }
      await flush();

      expect(worker.terminated).toBe(true);
      expect(request.snapshot.positions.byteLength).toBe(0);
      oldHandler?.({ data: {
        type: 'result', requestId: request.requestId, generation: request.generation,
        artifact: stale.bytes.buffer, identity: stale.identity,
        counters: { floatsVisited: 1, validationBytesVisited: 4, encoderArrayBytesCopied: 4,
          artifactChecksumCrcBytesVisited: stale.bytes.byteLength - 20,
          crcBytesVisited: stale.bytes.byteLength * 2 - 20, wholeScanCrcInvocations: 1,
          wholeScanCrcBytesVisited: stale.bytes.byteLength, bytesCopied: 4 },
      } } as MessageEvent<ScanEncodeWorkerResponse>);
      await flush();

      expect(stale.bytes.byteLength).toBe(0);
      expect(h.saveScan).not.toHaveBeenCalled();
      expect(h.saveOcc).not.toHaveBeenCalled();
      expect(h.savePrev).not.toHaveBeenCalled();
      if (action === 'Delete') expect(h.delScan).toHaveBeenCalledTimes(1);
    },
  );
});
