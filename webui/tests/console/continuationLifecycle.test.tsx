import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createElement } from 'react';
import { act, create, type ReactTestRenderer } from 'react-test-renderer';

const h = vi.hoisted(() => ({
  props: null as any,
  viewportProps: null as any,
  viewportEvents: [] as Array<{ snapshot: any; currentScanId: string | null }>,
  workers: [] as any[],
  outcomes: [] as any[],
  fetchRuns: vi.fn(), loadScan: vi.fn(), loadReg: vi.fn(), saveReg: vi.fn(), delReg: vi.fn(),
  saveScan: vi.fn(), saveOcc: vi.fn(), savePrev: vi.fn(), info: vi.fn(), delScan: vi.fn(), saveTags: vi.fn(), delTags: vi.fn(), loadTags: vi.fn(),
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
    saveSessionTagPositions: h.saveTags,
    deleteSessionTagPositions: h.delTags,
    loadSessionTagPositions: h.loadTags,
  };
});

vi.mock('../../src/components/XrRuntimePanel', () => ({ XrRuntimePanel: (props: unknown) => { h.props = props; return null; } }));
vi.mock('../../src/components/ScanPointCloudViewport', () => ({
  ScanPointCloudViewport: (props: any) => {
    h.viewportProps = props;
    h.viewportEvents.push({ snapshot: props.snapshot, currentScanId: props.currentScanId });
    return null;
  },
}));
vi.mock('../../src/xr/deviceComputeProfile', () => ({ deviceComputeProfilePromise: new Promise(() => undefined) }));
vi.mock('../../src/xr/continuationWorker?worker&inline', () => ({
  default: class {
    listeners = new Map<string, (event: any) => void>();
    request: any = null;
    terminated = false;
    constructor() { h.workers.push(this); }
    addEventListener(type: string, listener: (event: any) => void): void { this.listeners.set(type, listener); }
    removeEventListener(type: string): void { this.listeners.delete(type); }
    postMessage(request: any): void { this.request = request; }
    terminate(): void { this.terminated = true; }
    send(data: any): void { this.listeners.get('message')?.({ data }); }
    fail(message: string): void { this.listeners.get('error')?.({ message }); }
  },
}));

import App from '../../src/App';
import { clientDiagnostics } from '../../src/data/clientDiagnostics';
import { LocaleProvider } from '../../src/i18n';
import { decodeScanArtifact, encodeScanArtifact } from '../../src/xr/scanArtifact';
import { occupancyFrameIdForScan } from '../../src/xr/occupancySolverArtifact';
import { decodeTagPositions, encodeTagPositions, tagPositionsFromLayout } from '../../src/xr/fiducial/tagPositionsArtifact';
import { buildTagLayout } from '../../src/xr/fiducial/tagLayout';
import type { ScanPointCloudSnapshot } from '../../src/xr/webxrRuntime';
import { makeValidScanSnapshot } from '../xr/support/validScanSnapshot';

const mounted: ReactTestRenderer[] = [];
const device = { scan: null as Uint8Array | null, occupancy: null as Uint8Array | null, preview: null as Uint8Array | null };

function scanId(bytes: Uint8Array): string { return occupancyFrameIdForScan(bytes); }

async function flush(): Promise<void> {
  await act(async () => { for (let index = 0; index < 200; index += 1) await Promise.resolve(); });
}

async function mount(sessionId: string | null = 'S'): Promise<ReactTestRenderer> {
  let renderer!: ReactTestRenderer;
  await act(async () => {
    renderer = create(createElement(LocaleProvider, { initialLocale: 'en' }, createElement(App, { objectId: 'OBJ', sessionId, status: null, onContinuationOutcome: (outcome: unknown) => { h.outcomes.push(outcome); } })));
  });
  mounted.push(renderer);
  await flush();
  return renderer;
}

function changedSnapshot(): ScanPointCloudSnapshot {
  const source = makeValidScanSnapshot();
  const point = [0.5, 0.2, 0.3] as const;
  return {
    ...source,
    positions: new Float32Array(point),
    confidence: new Float32Array([0.92]),
    normals: new Float32Array([0, 1, 0]),
    normalConfidence: new Float32Array([0.88]),
    pointCount: 1,
    rawPointCount: 1,
    bounds: { min: point, max: point },
    fullBounds: { min: point, max: point },
    occupiedSurfacePositions: new Float32Array(point),
    manualControlPoints: [],
    operatorEdges: [],
  };
}

function oversizedSnapshot(): ScanPointCloudSnapshot {
  const source = makeValidScanSnapshot();
  const pointCount = 300_000;
  const positions = new Float32Array(pointCount * 3);
  const normals = new Float32Array(pointCount * 3);
  const confidence = new Float32Array(pointCount).fill(0.92);
  const normalConfidence = new Float32Array(pointCount).fill(0.88);
  for (let index = 0; index < pointCount; index += 1) {
    const offset = index * 3;
    positions[offset] = 0.1 + index * 1e-7;
    positions[offset + 1] = 0.2;
    positions[offset + 2] = 0.3;
    normals[offset + 1] = 1;
  }
  const bounds = { min: [0.1, 0.2, 0.3] as const, max: [0.13, 0.2, 0.3] as const };
  return {
    ...source,
    positions, normals, confidence, normalConfidence, pointCount,
    rawPointCount: pointCount, promotedPointCount: pointCount,
    maxPointCapacity: pointCount, memoryBudgetPoints: pointCount,
    bounds, fullBounds: bounds,
  };
}

function workerSuccess(snapshot: ScanPointCloudSnapshot, extensionToBase = { yaw: 0, tx: 0, ty: 0, tz: 0 }): any {
  return {
    ok: true,
    snapshot,
    diagnostics: {
      yawRadians: 0, translation: [0, 0, 0], inliers: 120, overlapRatio: 0.4,
      rmsMeters: 0.01, baseSampleCount: 120, extensionSampleCount: 120,
    },
    extensionToBase,
  };
}

async function openSavedBase(): Promise<{ renderer: ReactTestRenderer; baseSnapshot: ScanPointCloudSnapshot; baseBytes: Uint8Array; baseId: string }> {
  device.scan = encodeScanArtifact(makeValidScanSnapshot());
  device.occupancy = new Uint8Array([1]);
  device.preview = new Uint8Array([1]);
  const baseBytes = device.scan.slice();
  const baseId = scanId(baseBytes);
  const renderer = await mount();
  const baseSnapshot = h.viewportProps.snapshot as ScanPointCloudSnapshot;
  expect(decodeScanArtifact(device.scan).pointCount).toBe(1);
  expect(baseSnapshot.pointCount).toBe(1);
  return { renderer, baseSnapshot, baseBytes, baseId };
}

/** A tag lock as the scanner reports it: Continue is merged only through one (TASK 05). */
const LOCK = { yaw: 0, tx: 0, ty: 0, tz: 0 };

async function startContinue(extension = makeValidScanSnapshot()): Promise<any> {
  await act(async () => { expect(h.props.onScanIntent('continue')).toBe(true); h.props.onPointCloudReady(null); });
  await flush();
  await act(async () => { h.props.onPointCloudReady(extension, { relockTransform: LOCK }); });
  await flush();
  const worker = h.workers[h.workers.length - 1];
  if (worker === undefined) throw new Error('Continuation did not create a worker.');
  await act(async () => { h.props.onTagRegistrationResult(null, true); });
  return worker;
}

beforeEach(() => {
  h.props = null; h.viewportProps = null; h.viewportEvents = []; h.workers = []; h.outcomes = [];
  device.scan = null; device.occupancy = null; device.preview = null;
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
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
  h.loadTags.mockReset().mockResolvedValue(null);
  h.saveTags.mockReset().mockResolvedValue({ ok: true });
  h.delTags.mockReset().mockResolvedValue({ ok: true });
  h.saveScan.mockReset().mockImplementation(async (_object: string, _session: string, bytes: Uint8Array) => {
    device.scan = bytes.slice(); device.occupancy = null; device.preview = null;
    return { ok: true, action: 'scan', id: scanId(bytes) };
  });
  h.saveOcc.mockReset().mockImplementation(async (_object: string, _session: string, bytes: Uint8Array) => { device.occupancy = bytes.slice(); return { ok: true, action: 'occupancy' }; });
  h.savePrev.mockReset().mockImplementation(async (_object: string, _session: string, bytes: Uint8Array) => { device.preview = bytes.slice(); return { ok: true, action: 'preview' }; });
  h.info.mockReset().mockImplementation(async (_object: string, _session: string, kind: 'scan' | 'occupancy' | 'preview') => {
    const bytes = kind === 'scan' ? device.scan : kind === 'occupancy' ? device.occupancy : device.preview;
    if (bytes === null) return { present: false, bytes: 0 };
    const currentId = device.scan === null ? null : scanId(device.scan);
    return kind === 'scan'
      ? { present: true, bytes: bytes.byteLength, id: currentId }
      : { present: true, bytes: bytes.byteLength, id: null, sourceId: currentId };
  });
});

afterEach(() => {
  for (const renderer of [...mounted]) { act(() => renderer.unmount()); mounted.splice(mounted.indexOf(renderer), 1); }
  vi.unstubAllGlobals();
});

describe('App off-thread continuation lifecycle (MAX-393)', () => {
  it('hands the tag-derived transform of a locked Continue to the worker as its registration seed (issue #45)', async () => {
    const T = { yaw: 0.31, tx: 1.25, ty: -0.5, tz: 0.02 };
    await openSavedBase();
    await act(async () => { expect(h.props.onScanIntent('continue')).toBe(true); h.props.onPointCloudReady(null); });
    await flush();
    await act(async () => { h.props.onPointCloudReady(makeValidScanSnapshot(), { relockTransform: T }); });
    await flush();
    const locked = h.workers[h.workers.length - 1];
    expect(locked.request.options.seed).toEqual(T);
  });

  it('CONT-02/03/12: a Continue that did not lock (one tag, two tags, none) is REJECTED with the concrete reason, creates no worker, keeps A, and ends in exactly one terminal state', async () => {
    for (const [found, code] of [[0, 'tags_not_found'], [1, 'tags_not_found'], [2, 'tags_not_found']] as const) {
      h.workers = []; h.outcomes = [];
      const captured: Array<{ name: string; bytes: Uint8Array }> = [];
      (globalThis as { __phoneLab?: unknown }).__phoneLab = { capture: (name: string, bytes: Uint8Array) => captured.push({ name, bytes }) };
      try {
        const { baseSnapshot } = await openSavedBase();
        await act(async () => { expect(h.props.onScanIntent('continue')).toBe(true); h.props.onPointCloudReady(null); });
        await flush();
        await act(async () => { h.props.onPointCloudReady(makeValidScanSnapshot(), { relockTransform: null, relock: { baseTags: 4, found, needed: 3, reason: 'need_more_tags' } }); });
        await flush();
        expect(h.workers, `${found} tags: no registration by overlap alone`).toHaveLength(0);
        expect(h.outcomes).toHaveLength(1);
        expect(h.outcomes[0]).toMatchObject({ state: 'rejected', code, messageKey: 'scan.error.continue_requires_tags' });
        expect(h.outcomes[0].params.detail).toContain(`${found} of 3`);
        expect(h.viewportProps.snapshot, 'the saved scan is untouched').toBe(baseSnapshot);
        expect(h.saveScan).not.toHaveBeenCalled();
        const cloud = captured.find((entry) => entry.name === 'continuation_b_rejected');
        expect(cloud, 'a diagnostic copy of B is kept').toBeDefined();
        expect(JSON.parse(new TextDecoder().decode(captured.find((entry) => entry.name === 'continuation_seed_json')!.bytes))).toMatchObject({ state: 'rejected', code, relock: { found } });
      } finally { delete (globalThis as { __phoneLab?: unknown }).__phoneLab; }
      await act(async () => { for (const renderer of [...mounted]) renderer.unmount(); mounted.length = 0; });
      h.props = null;
    }
  });

  it('CONT-04: tags that were found but are geometrically unusable (collinear, moved, poor fit) give their own reason', async () => {
    for (const reason of ['collinear', 'distance_mismatch', 'residual_too_high'] as const) {
      h.workers = []; h.outcomes = [];
      await openSavedBase();
      await act(async () => { expect(h.props.onScanIntent('continue')).toBe(true); h.props.onPointCloudReady(null); });
      await flush();
      await act(async () => { h.props.onPointCloudReady(makeValidScanSnapshot(), { relockTransform: null, relock: { baseTags: 4, found: 3, needed: 3, reason } }); });
      await flush();
      expect(h.workers).toHaveLength(0);
      expect(h.outcomes).toHaveLength(1);
      expect(h.outcomes[0]).toMatchObject({ state: 'rejected', code: reason });
      await act(async () => { for (const renderer of [...mounted]) renderer.unmount(); mounted.length = 0; });
      h.props = null;
    }
  });

  it('a saved scan with fewer than three usable tags cannot be continued: the reason names the saved scan, not the new one', async () => {
    await openSavedBase();
    await act(async () => { expect(h.props.onScanIntent('continue')).toBe(true); h.props.onPointCloudReady(null); });
    await flush();
    await act(async () => { h.props.onPointCloudReady(makeValidScanSnapshot(), { relockTransform: null, relock: { baseTags: 2, found: 0, needed: 3, reason: 'need_more_tags' } }); });
    await flush();
    expect(h.outcomes).toHaveLength(1);
    expect(h.outcomes[0]).toMatchObject({ state: 'rejected', code: 'base_tags_insufficient' });
    expect(h.outcomes[0].params.detail).toContain('only 2');
  });

  it('CONT-16: after a rejection the same workflow can Continue again, and that one merges', async () => {
    const { renderer } = await openSavedBase();
    void renderer;
    await act(async () => { expect(h.props.onScanIntent('continue')).toBe(true); h.props.onPointCloudReady(null); });
    await flush();
    await act(async () => { h.props.onPointCloudReady(makeValidScanSnapshot(), { relockTransform: null, relock: { baseTags: 4, found: 1, needed: 3, reason: 'need_more_tags' } }); });
    await flush();
    expect(h.outcomes).toHaveLength(1);
    const worker = await startContinue();
    expect(worker.request.options.seed).toEqual(LOCK);
    await act(async () => { worker.send({ type: 'result', requestId: worker.request.requestId, generation: worker.request.generation, result: workerSuccess(changedSnapshot()) }); });
    await flush();
    expect(h.outcomes.map((o) => o.state)).toEqual(['rejected', 'success']);
  });

  it('reopens a scan that is still staged (no recording yet) with the saved scan on screen and Continue available (audit 2026-11-22)', async () => {
    device.scan = encodeScanArtifact(makeValidScanSnapshot());
    await mount(null);
    expect((h.viewportProps?.snapshot as ScanPointCloudSnapshot | undefined)?.pointCount).toBe(1);
    expect(h.loadScan).toHaveBeenCalledWith('OBJ', 'staging');
    await act(async () => { expect(h.props.onScanIntent('continue')).toBe(true); });
  });

  it('VOX-09/10/11/12: choosing a voxel representation tells the viewport at once (even while its data is loading or missing), Mesh restores it, and a late answer of a representation that is no longer chosen is not drawn', async () => {
    const { renderer } = await openSavedBase();
    const choose = async (value: string) => { await act(async () => { renderer.root.findByType('select').props.onChange({ target: { value } }); }); await flush(); };
    expect(h.viewportProps.voxelsSelected).toBe(false);
    expect(h.viewportProps.esp32Voxels ?? null).toBeNull();
    await choose('esp32-preview');
    expect(h.viewportProps.voxelsSelected, 'the device holds no preview: the canvas must not keep drawing the mesh').toBe(true);
    expect(h.viewportProps.esp32Voxels ?? null).toBeNull();
    await choose('esp32-solver');
    expect(h.viewportProps.voxelsSelected).toBe(true);
    await choose('mesh');
    expect(h.viewportProps.voxelsSelected).toBe(false);
    expect(h.viewportProps.esp32Voxels ?? null).toBeNull();
    /* rapid switching: the last choice wins */
    await act(async () => {
      const select = renderer.root.findByType('select');
      select.props.onChange({ target: { value: 'esp32-preview' } });
      select.props.onChange({ target: { value: 'esp32-solver' } });
      select.props.onChange({ target: { value: 'mesh' } });
    });
    await flush();
    expect(h.viewportProps.voxelsSelected).toBe(false);
  });

  it('offers the mesh computed for the saved scan on screen to the scanner, and none for another scan (Continue draws it over the camera)', async () => {
    const { baseSnapshot } = await openSavedBase();
    expect(h.props.getReferenceMesh()).toBeNull();                                                 /* the mesh of this scan was never computed */
    const triangles = Float32Array.from([0, 0, 0, 1, 0, 0, 0, 1, 0]);
    expect(typeof h.viewportProps.onMeshReady).toBe('function');
    act(() => { h.viewportProps.onMeshReady(baseSnapshot, triangles, 0.048); });
    expect(h.props.getReferenceMesh()).toBe(triangles);
    /* a large surface is offered as its lighter copy, and a repeated notice of the same mesh does not undo that */
    const wall: number[] = [];
    for (let y = 0; y < 20; y += 1) for (let x = 0; x < 20; x += 1) {
      const [x0, x1, y0, y1] = [x, x + 1, y, y + 1].map((v) => v * 0.04);
      wall.push(x0, y0, 0, x1, y0, 0, x1, y1, 0, x0, y0, 0, x1, y1, 0, x0, y1, 0);
    }
    const surface = Float32Array.from(wall);
    act(() => { h.viewportProps.onMeshReady(baseSnapshot, surface, 0.048); });
    await vi.waitFor(() => expect(h.props.getReferenceMesh().length).toBeLessThan(surface.length / 2));
    const lighter = h.props.getReferenceMesh();
    act(() => { h.viewportProps.onMeshReady(baseSnapshot, surface, 0.048); });
    expect(h.props.getReferenceMesh()).toBe(lighter);
    act(() => { h.viewportProps.onMeshReady({ ...baseSnapshot }, Float32Array.from([9, 9, 9, 9, 9, 9, 9, 9, 9]), 0.048); });
    expect(h.props.getReferenceMesh(), 'a mesh that belongs to another snapshot object is never offered').toBeNull();
  });

  it('hands the real B cloud of a rejected Continue to the Phone Lab capture hook, and does nothing when there is no hook (issue #45)', async () => {
    const captured: Array<{ name: string; bytes: Uint8Array }> = [];
    const reject = async (): Promise<void> => {
      await openSavedBase();
      const extension = makeValidScanSnapshot();
      const worker = await startContinue(extension);
      const request = worker.request;
      await act(async () => { worker.send({ type: 'result', requestId: request.requestId, generation: request.generation, result: { ok: false, reason: 'overlap', diagnostics: null } }); });
      await flush();
    };
    (globalThis as { __phoneLab?: unknown }).__phoneLab = { capture: (name: string, bytes: Uint8Array) => captured.push({ name, bytes }) };
    try { await reject(); } finally { delete (globalThis as { __phoneLab?: unknown }).__phoneLab; }
    const cloud = captured.find((entry) => entry.name === 'continuation_b_rejected');
    expect(cloud).toBeDefined();
    expect(decodeScanArtifact(cloud!.bytes).pointCount).toBe(makeValidScanSnapshot().pointCount);       /* the exact B, decodable by the replay tests */
    expect(JSON.parse(new TextDecoder().decode(captured.find((entry) => entry.name === 'continuation_seed_json')!.bytes))).toMatchObject({ reason: 'overlap' });

    captured.length = 0;
    await act(async () => { for (const renderer of [...mounted]) renderer.unmount(); mounted.length = 0; });
    h.props = null; h.workers = [];
    await reject();
    expect(captured).toEqual([]);                                  /* production bundle: no hook, nothing captured, nothing thrown */
  });

  it('keeps durable A visible while registration is pending, then preserves A after a worker error', async () => {
    const { renderer, baseSnapshot, baseBytes, baseId } = await openSavedBase();
    const worker = await startContinue();

    expect(worker.request.type).toBe('continue');
    expect(worker.request.base.positions).not.toBe(baseSnapshot.positions);
    expect(h.viewportProps.snapshot).toBe(baseSnapshot);
    expect(h.saveScan).not.toHaveBeenCalled();

    await act(async () => { worker.fail('worker failed'); });
    await flush();

    expect(worker.terminated).toBe(true);
    expect(h.saveScan).not.toHaveBeenCalled();
    expect(h.saveOcc).not.toHaveBeenCalled();
    expect(h.savePrev).not.toHaveBeenCalled();
    expect(h.viewportProps.snapshot).toBe(baseSnapshot);
    expect(h.viewportProps.currentScanId).toBe(baseId);
    expect(device.scan).toEqual(baseBytes);
    const rendered = JSON.stringify(renderer.toJSON());
    expect(rendered).toContain('Scan merge failed: worker failed. The saved scan is unchanged.');
    expect(h.outcomes).toHaveLength(1);
    expect(h.outcomes[0]).toMatchObject({ state: 'error', code: 'worker_error' });
    expect(rendered).toContain('3D scan restored from SD · OBJ');
  });

  it('terminates and releases request copies when Fresh supersedes the worker', async () => {
    const { baseSnapshot } = await openSavedBase();
    const worker = await startContinue();
    const requestBase = worker.request.base as ScanPointCloudSnapshot;
    expect(requestBase.positions.byteLength).toBeGreaterThan(0);

    await act(async () => { expect(h.props.onScanIntent('fresh')).toBe(true); });
    await flush();

    expect(worker.terminated).toBe(true);
    expect(requestBase.positions.byteLength).toBe(0);
    expect(baseSnapshot.positions.byteLength).toBeGreaterThan(0);
    expect(h.saveScan).not.toHaveBeenCalled();
    expect(h.viewportProps.snapshot).toBe(baseSnapshot);
  });

  it('terminates and releases request copies when the saved scan is deleted', async () => {
    const { baseBytes } = await openSavedBase();
    const worker = await startContinue();
    const request = worker.request;

    await act(async () => { h.props.onDeleteSavedScan(); });
    await flush();

    expect(worker.terminated).toBe(true);
    expect(request.base.positions.byteLength).toBe(0);
    expect(h.delScan).toHaveBeenCalledTimes(1);
    expect(h.saveScan).not.toHaveBeenCalled();
    expect(h.saveOcc).not.toHaveBeenCalled();
    expect(device.scan).toEqual(baseBytes);
  });

  it('cancels and releases the worker when the object or session owner changes', async () => {
    const { renderer, baseBytes } = await openSavedBase();
    const worker = await startContinue();
    const request = worker.request;

    await act(async () => {
      renderer.update(createElement(LocaleProvider, { initialLocale: 'en' }, createElement(App, { objectId: 'OBJ-2', sessionId: 'S-2', status: null })));
    });
    await flush();

    expect(worker.terminated).toBe(true);
    expect(request.base.positions.byteLength).toBe(0);
    expect(device.scan).toEqual(baseBytes);
    expect(h.saveScan).not.toHaveBeenCalled();
  });

  it('disposes the worker and releases copies when App unmounts', async () => {
    const { renderer } = await openSavedBase();
    const worker = await startContinue();
    const request = worker.request;

    act(() => { renderer.unmount(); });
    mounted.splice(mounted.indexOf(renderer), 1);
    await flush();

    expect(worker.terminated).toBe(true);
    expect(request.base.positions.byteLength).toBe(0);
    expect(h.saveScan).not.toHaveBeenCalled();
  });

  it('CONT-13 / TASK 05: closing the scanner mid-merge ends the Continue as CANCELLED, once, with a diagnostic copy of B (the old code ended it in silence and showed the saved scan)', async () => {
    const captured: Array<{ name: string }> = [];
    (globalThis as { __phoneLab?: unknown }).__phoneLab = { capture: (name: string) => captured.push({ name }) };
    try {
      const { renderer } = await openSavedBase();
      const worker = await startContinue();
      expect(h.outcomes, 'running: no terminal state yet').toHaveLength(0);
      act(() => { renderer.unmount(); });
      mounted.splice(mounted.indexOf(renderer), 1);
      await flush();
      expect(worker.terminated).toBe(true);
      expect(h.outcomes).toHaveLength(1);
      expect(h.outcomes[0]).toMatchObject({ state: 'cancelled', code: 'viewer_closed', messageKey: 'scan.continuation.cancelled_viewer_closed' });
      expect(captured.map((entry) => entry.name)).toEqual(expect.arrayContaining(['continuation_b_rejected', 'continuation_seed_json']));
    } finally { delete (globalThis as { __phoneLab?: unknown }).__phoneLab; }
  });

  it('a newer scan, a deleted scan and an owner change each end the running Continue as cancelled exactly once, and the late worker answer adds nothing', async () => {
    const { baseSnapshot } = await openSavedBase();
    const worker = await startContinue();
    await act(async () => { expect(h.props.onScanIntent('fresh')).toBe(true); });
    await flush();
    expect(h.outcomes.map((o) => o.state)).toEqual(['cancelled']);
    /* the cancelled worker answers anyway (stale callback): nothing is added, nothing is saved, the saved scan stays */
    await act(async () => { worker.send({ type: 'result', requestId: worker.request.requestId, generation: worker.request.generation, result: workerSuccess(changedSnapshot()) }); });
    await flush();
    expect(h.outcomes.map((o) => o.state)).toEqual(['cancelled']);
    expect(h.saveScan).not.toHaveBeenCalled();
    expect(h.viewportProps.snapshot).toBe(baseSnapshot);
  });

  it('CONT-15: a worker that never answers is stopped by the timeout and ends the Continue as an ERROR with its reason', async () => {
    vi.useFakeTimers();
    try {
      await openSavedBase();
      await act(async () => { expect(h.props.onScanIntent('continue')).toBe(true); h.props.onPointCloudReady(null); });
      await flush();
      await act(async () => { h.props.onPointCloudReady(makeValidScanSnapshot(), { relockTransform: LOCK }); });
      const worker = h.workers[h.workers.length - 1];
      expect(worker.terminated).toBe(false);
      await act(async () => { await vi.advanceTimersByTimeAsync(181_000); });
      await flush();
      expect(worker.terminated).toBe(true);
      expect(h.outcomes).toHaveLength(1);
      expect(h.outcomes[0]).toMatchObject({ state: 'error', code: 'worker_timeout', messageKey: 'scan.continuation.timeout' });
    } finally { vi.useRealTimers(); }
  });

  it('TASK 11: STARTED names the operation and the lock, and exactly one of REGISTERED / REJECTED / CANCELLED / FAILED follows with a real reason', async () => {
    const info = vi.spyOn(clientDiagnostics, 'info').mockImplementation(() => undefined);
    const warn = vi.spyOn(clientDiagnostics, 'warn').mockImplementation(() => undefined);
    const terminal = (): string[] => [...info.mock.calls, ...warn.mock.calls].map((call) => String(call[0])).filter((name) => /^SCAN_CONTINUATION_(REGISTERED|REJECTED|CANCELLED|FAILED)$/.test(name));
    try {
      await openSavedBase();
      /* gate rejection */
      await act(async () => { expect(h.props.onScanIntent('continue')).toBe(true); h.props.onPointCloudReady(null); });
      await flush();
      await act(async () => { h.props.onPointCloudReady(makeValidScanSnapshot(), { relockTransform: null, relock: { baseTags: 4, found: 2, needed: 3, reason: 'need_more_tags' } }); });
      await flush();
      expect(info).toHaveBeenCalledWith('SCAN_CONTINUATION_STARTED', expect.objectContaining({ objectId: 'OBJ', locked: false, tagsInSavedScan: 4, tagsFound: 2, tagsNeeded: 3, lockReason: 'need_more_tags' }));
      expect(warn).toHaveBeenCalledWith('SCAN_CONTINUATION_REJECTED', expect.objectContaining({ reason: 'tags_not_found' }));
      expect(terminal()).toEqual(['SCAN_CONTINUATION_REJECTED']);
      /* worker error -> FAILED, once */
      info.mockClear(); warn.mockClear();
      const worker = await startContinue();
      await act(async () => { worker.fail('boom'); });
      await flush();
      expect(warn).toHaveBeenCalledWith('SCAN_CONTINUATION_FAILED', { objectId: 'OBJ', code: 'worker_error' });
      expect(terminal().filter((name) => name === 'SCAN_CONTINUATION_FAILED')).toHaveLength(1);
      /* superseded -> CANCELLED, once */
      info.mockClear(); warn.mockClear();
      await startContinue();
      await act(async () => { expect(h.props.onScanIntent('fresh')).toBe(true); });
      await flush();
      expect(warn).toHaveBeenCalledWith('SCAN_CONTINUATION_CANCELLED', { objectId: 'OBJ', code: 'cancelled' });
      expect(terminal().filter((name) => name === 'SCAN_CONTINUATION_CANCELLED')).toHaveLength(1);
    } finally { info.mockRestore(); warn.mockRestore(); }
  });

  it('CONT-24: whatever the terminal state, the saved scan is never written or replaced', async () => {
    const { baseSnapshot, baseBytes } = await openSavedBase();
    const worker = await startContinue();
    await act(async () => { worker.fail('boom'); });
    await flush();
    expect(h.outcomes[0]).toMatchObject({ state: 'error' });
    expect(h.saveScan).not.toHaveBeenCalled();
    expect(device.scan).toEqual(baseBytes);
    expect(h.viewportProps.snapshot).toBe(baseSnapshot);
  });

  it('keeps A durable and generates no derivatives when merged artifact encoding exceeds 8 MiB', async () => {
    const { renderer, baseSnapshot, baseBytes, baseId } = await openSavedBase();
    const worker = await startContinue();
    const request = worker.request;
    const oversized = oversizedSnapshot();
    expect(oversized.positions.byteLength + oversized.confidence.byteLength + oversized.normals.byteLength +
      oversized.normalConfidence!.byteLength).toBeGreaterThan(8 * 1024 * 1024);
    await act(async () => {
      worker.send({ type: 'result', requestId: request.requestId, generation: request.generation, result: workerSuccess(oversized) });
    });
    await flush();

    expect(h.saveScan).not.toHaveBeenCalled();
    expect(h.saveOcc).not.toHaveBeenCalled();
    expect(h.savePrev).not.toHaveBeenCalled();
    expect(device.scan).toEqual(baseBytes);
    expect(h.viewportProps.snapshot).toBe(baseSnapshot);
    expect(h.viewportProps.currentScanId).toBe(baseId);
    expect(JSON.stringify(renderer.toJSON())).toContain('Could not prepare the 3D scan for saving.');
  });

  it.each(['registration', 'stable', 'occupancy', 'tsdf'] as const)(
    'keeps A and issues no uploads when the worker fails during %s', async (phase) => {
      const { baseSnapshot, baseBytes, baseId } = await openSavedBase();
      const worker = await startContinue();
      const request = worker.request;
      await act(async () => {
        worker.send({ type: 'phase', requestId: request.requestId, generation: request.generation, phase });
      });
      await flush();
      await act(async () => { worker.fail(`${phase} failed`); });
      await flush();

      expect(worker.terminated).toBe(true);
      expect(request.base.positions.byteLength).toBe(0);
      expect(device.scan).toEqual(baseBytes);
      expect(h.viewportProps.snapshot).toBe(baseSnapshot);
      expect(h.viewportProps.currentScanId).toBe(baseId);
      expect(h.saveScan).not.toHaveBeenCalled();
      expect(h.saveOcc).not.toHaveBeenCalled();
      expect(h.savePrev).not.toHaveBeenCalled();
    },
  );

  it.each(['registration', 'stable', 'occupancy', 'tsdf'] as const)(
    'terminates and keeps A when the scanner leaves during %s', async (phase) => {
      const { baseSnapshot, baseBytes, baseId } = await openSavedBase();
      const worker = await startContinue();
      const request = worker.request;
      await act(async () => {
        worker.send({ type: 'phase', requestId: request.requestId, generation: request.generation, phase });
      });
      await flush();
      await act(async () => { h.props.onSessionActiveChange(false); });
      await flush();

      expect(worker.terminated).toBe(true);
      expect(request.base.positions.byteLength).toBe(0);
      expect(device.scan).toEqual(baseBytes);
      expect(h.viewportProps.snapshot).toBe(baseSnapshot);
      expect(h.viewportProps.currentScanId).toBe(baseId);
      expect(h.saveScan).not.toHaveBeenCalled();
      expect(h.saveOcc).not.toHaveBeenCalled();
      expect(h.savePrev).not.toHaveBeenCalled();
    },
  );

  it('restores A after transport failure and switches durable authority once after confirmed success', async () => {
    const failed = await openSavedBase();
    h.saveScan.mockRejectedValueOnce(new Error('transport down'));
    const failedWorker = await startContinue();
    const failedRequest = failedWorker.request;
    await act(async () => {
      failedWorker.send({ type: 'result', requestId: failedRequest.requestId, generation: failedRequest.generation, result: workerSuccess(changedSnapshot()) });
    });
    await flush();

    expect(h.saveScan).toHaveBeenCalledTimes(1);
    expect(h.saveOcc).not.toHaveBeenCalled();
    expect(h.savePrev).not.toHaveBeenCalled();
    expect(device.scan).toEqual(failed.baseBytes);
    expect(h.viewportProps.snapshot).toBe(failed.baseSnapshot);
    expect(h.viewportProps.currentScanId).toBe(failed.baseId);

    act(() => { failed.renderer.unmount(); });
    mounted.splice(mounted.indexOf(failed.renderer), 1);
    h.viewportEvents = [];
    const successful = await openSavedBase();
    const successWorker = await startContinue();
    const successRequest = successWorker.request;
    const merged = changedSnapshot();
    const candidateId = scanId(encodeScanArtifact(merged));
    expect(h.viewportProps.snapshot).toBe(successful.baseSnapshot);
    expect(h.viewportEvents.some((event) => event.snapshot === merged && event.currentScanId === null)).toBe(false);

    await act(async () => {
      successWorker.send({ type: 'result', requestId: successRequest.requestId, generation: successRequest.generation, result: workerSuccess(merged) });
    });
    await flush();

    expect(device.scan).not.toEqual(successful.baseBytes);
    expect(h.viewportProps.snapshot).toBe(merged);
    expect(h.viewportProps.currentScanId).toBe(candidateId);
    expect(h.saveScan).toHaveBeenCalledTimes(2);
    const durableTransitions = h.viewportEvents
      .map((event) => event.currentScanId)
      .filter((id, index, values) => id !== values[index - 1]);
    expect(durableTransitions.filter((id) => id === candidateId)).toHaveLength(1);
    expect(h.saveOcc).toHaveBeenCalledTimes(1);
    expect(h.savePrev).toHaveBeenCalledTimes(1);
  });
});

describe('tag sidecar ownership while a continuation is merged (issues #37, #38)', () => {
  const tag = (tagId: number, node: string, xr: [number, number, number]) =>
    ({ tagId, node, positionScan: xr, spreadM: 0.003, observationCount: 8, independentViewCount: 4, confirmed: true });
  const MAIN = tag(0, 'MAIN', [1, 1, -1]);
  const G1 = tag(1, 'G1', [1.5, 1, -1]);
  const G2 = tag(2, 'G2', [1, 1, -1.8]);
  const G3 = tag(3, 'G3', [0.9, 1.2, -1.5]);
  const T_BA = { yaw: Math.PI / 2, tx: 3, ty: -2, tz: 0.5 };

  it('writes and deletes nothing against durable A while the merge is pending, and nothing after it is rejected', async () => {
    const base = await openSavedBase();
    const worker = await startContinue();
    await act(async () => { h.props.onTagAnchors([MAIN, G1, G2, G3]); });
    await flush();
    expect(h.saveTags).not.toHaveBeenCalled();
    expect(h.delTags).not.toHaveBeenCalled();

    const request = worker.request;
    await act(async () => { worker.send({ type: 'result', requestId: request.requestId, generation: request.generation, result: { ok: false, reason: 'overlap', diagnostics: null } }); });
    await flush();
    expect(h.saveTags).not.toHaveBeenCalled();
    expect(h.delTags).not.toHaveBeenCalled();
    expect(device.scan).toEqual(base.baseBytes);
    expect(h.viewportProps.currentScanId).toBe(base.baseId);
  });

  it('does not delete A\'s saved layout when the extension saw no tags (cold page: no layout remembered in memory)', async () => {
    const base = await openSavedBase();
    const worker = await startContinue();
    await act(async () => { h.props.onTagAnchors([]); });
    await flush();
    expect(h.delTags).not.toHaveBeenCalled();
    const request = worker.request;
    await act(async () => { worker.send({ type: 'error', requestId: request.requestId, generation: request.generation, message: 'worker crashed' }); });
    await flush();
    expect(h.saveTags).not.toHaveBeenCalled();
    expect(h.delTags).not.toHaveBeenCalled();
    expect(device.scan).toEqual(base.baseBytes);
  });

  it('saves the layout of an accepted merge in the frame of the merged scan A, under the merged scan id (not in frame B under A)', async () => {
    await openSavedBase();
    const worker = await startContinue();
    await act(async () => { h.props.onTagAnchors([MAIN, G1, G2, G3]); });
    await flush();
    expect(h.saveTags).not.toHaveBeenCalled();                       /* still pending: nothing is written under the old id */

    const merged = changedSnapshot();
    const mergedId = scanId(encodeScanArtifact(merged));
    const request = worker.request;
    await act(async () => { worker.send({ type: 'result', requestId: request.requestId, generation: request.generation, result: workerSuccess(merged, T_BA) }); });
    await flush();

    expect(h.saveTags).toHaveBeenCalledTimes(1);
    const [objectId, , json] = h.saveTags.mock.calls[0] as [string, string, string];
    expect(objectId).toBe('OBJ');
    const saved = decodeTagPositions(json);
    expect(saved.scan_frame_id).toBe(mergedId);
    expect(saved.main_scan_position_mm.map(Math.round)).toEqual([2000, -1000, 1500]);
    const byNode = new Map(saved.nodes.map((node) => [node.node_id, [Math.round(node.x_mm) + 0, Math.round(node.y_mm) + 0, Math.round(node.z_mm) + 0]]));
    expect(byNode.get(1)).toEqual([0, 500, 0]);
    expect(byNode.get(2)).toEqual([-800, 0, 0]);
    expect(byNode.get(3)).toEqual([-500, -100, 200]);
    expect(h.delTags).not.toHaveBeenCalled();
  });
});

describe('a continuation inherits the saved tag layout of A (issues #37, #38)', () => {
  const tag = (tagId: number, node: string, xr: [number, number, number]) =>
    ({ tagId, node, positionScan: xr, spreadM: 0.003, observationCount: 8, independentViewCount: 4, confirmed: true });
  const MAIN = tag(0, 'MAIN', [1, 1, -1]);
  const G1 = tag(1, 'G1', [1.5, 1, -1]);
  const G2 = tag(2, 'G2', [1, 1, -1.8]);
  const G3 = tag(3, 'G3', [0.9, 1.2, -1.5]);

  /** The layout the ESP32 holds for durable A, as read back after a page reload. */
  function savedLayoutFor(baseId: string, anchors: ReturnType<typeof tag>[]): string {
    const built = buildTagLayout(anchors);
    if (built.layout === null) throw new Error('fixture layout expected');
    return encodeTagPositions(tagPositionsFromLayout(built.layout, baseId, 1));
  }

  it('draws the sensors from the saved layout when a scan comes back from the ESP32, with no live anchors (phone run 2026-11-22: no sensors in the viewport)', async () => {
    const bytes = encodeScanArtifact(makeValidScanSnapshot());
    const baseId = scanId(bytes);
    device.scan = bytes;
    h.loadTags.mockResolvedValue(savedLayoutFor(baseId, [MAIN, G1, G2, G3]));
    await mount(null);
    const markers = (h.viewportProps as { sensorMarkers: Array<{ node: string; positionScanM: readonly number[] | null; tagId: number | null }> }).sensorMarkers;
    const placed = markers.filter((marker) => marker.positionScanM !== null);       /* an unplaced channel-sounding entry may stand beside them */
    expect(placed.map((marker) => marker.node).sort()).toEqual(['G1', 'G2', 'G3', 'MAIN']);
    expect(placed.every((marker) => marker.positionScanM!.every(Number.isFinite))).toBe(true);
    /* a layout saved for ANOTHER scan must not put sensors into this one */
    h.loadTags.mockResolvedValue(savedLayoutFor('scan-crc32:deadbeef', [MAIN, G1, G2, G3]));
    for (const renderer of [...mounted]) act(() => renderer.unmount());
    mounted.length = 0;
    await mount(null);
    expect((h.viewportProps as { sensorMarkers: Array<{ positionScanM: unknown }> }).sensorMarkers.filter((marker) => marker.positionScanM !== null)).toEqual([]);
  });

  it('a staged scan reopened from the workflow brings its saved tags to the scanner as re-lock anchors (audit 2026-11-22)', async () => {
    const bytes = encodeScanArtifact(makeValidScanSnapshot());
    const baseId = scanId(bytes);
    device.scan = bytes;
    h.loadTags.mockResolvedValue(savedLayoutFor(baseId, [MAIN, G1, G2, G3]));
    await mount(null);
    expect(h.loadTags).toHaveBeenCalledWith('OBJ', 'staging');
    const stored = h.props.getRelockBase();
    expect(stored).not.toBeNull();
    expect(stored.scan_frame_id).toBe(baseId);
  });

  async function mergeWithExtensionTags(extensionTags: ReturnType<typeof tag>[]): Promise<string> {
    const baseId = scanId(encodeScanArtifact(makeValidScanSnapshot()));
    h.loadTags.mockResolvedValue(savedLayoutFor(baseId, [MAIN, G1, G2]));
    await openSavedBase();
    const worker = await startContinue();
    await act(async () => { h.props.onTagAnchors(extensionTags); });
    await flush();
    const merged = changedSnapshot();
    const request = worker.request;
    await act(async () => { worker.send({ type: 'result', requestId: request.requestId, generation: request.generation, result: workerSuccess(merged) }); });
    await flush();
    return scanId(encodeScanArtifact(merged));
  }

  it('carries A\'s layout over to the merged scan when the extension saw no tags (nothing is deleted)', async () => {
    const mergedId = await mergeWithExtensionTags([]);
    expect(h.delTags).not.toHaveBeenCalled();
    expect(h.saveTags).toHaveBeenCalledTimes(1);
    const saved = decodeTagPositions((h.saveTags.mock.calls[0] as [string, string, string])[2]);
    const original = decodeTagPositions(savedLayoutFor('scan-crc32:aaaaaaaa', [MAIN, G1, G2]));
    expect(saved.scan_frame_id).toBe(mergedId);
    expect(saved.nodes).toEqual(original.nodes);
    expect(saved.main_scan_position_mm).toEqual(original.main_scan_position_mm);
  });

  it('keeps A\'s layout when the extension only saw a weaker one (too few tags for a layout)', async () => {
    const mergedId = await mergeWithExtensionTags([MAIN, G1]);
    expect(h.delTags).not.toHaveBeenCalled();
    expect(h.saveTags).toHaveBeenCalledTimes(1);
    const saved = decodeTagPositions((h.saveTags.mock.calls[0] as [string, string, string])[2]);
    expect(saved.scan_frame_id).toBe(mergedId);
    expect(saved.nodes.map((node) => node.node_id)).toEqual([0, 1, 2]);
  });

  it('replaces A\'s layout by a stronger one found in the extension (more tags), in the merged frame', async () => {
    const mergedId = await mergeWithExtensionTags([MAIN, G1, G2, G3]);
    expect(h.delTags).not.toHaveBeenCalled();
    expect(h.saveTags).toHaveBeenCalledTimes(1);
    const saved = decodeTagPositions((h.saveTags.mock.calls[0] as [string, string, string])[2]);
    expect(saved.scan_frame_id).toBe(mergedId);
    expect(saved.nodes.map((node) => node.node_id)).toEqual([0, 1, 2, 3]);
  });

  it('after a page reload a weaker look at the same scan never replaces or removes the saved layout (the layout is read from the device)', async () => {
    const baseId = scanId(encodeScanArtifact(makeValidScanSnapshot()));
    h.loadTags.mockResolvedValue(savedLayoutFor(baseId, [MAIN, G1, G2, G3]));
    await openSavedBase();
    expect(h.loadTags).toHaveBeenCalledTimes(1);
    const worker = await startContinue();
    await act(async () => { h.props.onTagAnchors([MAIN, G1]); });
    const request = worker.request;
    await act(async () => { worker.send({ type: 'result', requestId: request.requestId, generation: request.generation, result: { ok: false, reason: 'overlap', diagnostics: null } }); });
    await flush();
    expect(h.saveTags).not.toHaveBeenCalled();
    expect(h.delTags).not.toHaveBeenCalled();
  });
});
