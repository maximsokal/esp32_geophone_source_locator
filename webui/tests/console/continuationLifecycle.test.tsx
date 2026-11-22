import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createElement } from 'react';
import { act, create, type ReactTestRenderer } from 'react-test-renderer';

const h = vi.hoisted(() => ({
  props: null as any,
  viewportProps: null as any,
  viewportEvents: [] as Array<{ snapshot: any; currentScanId: string | null }>,
  workers: [] as any[],
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

async function mount(): Promise<ReactTestRenderer> {
  let renderer!: ReactTestRenderer;
  await act(async () => {
    renderer = create(createElement(LocaleProvider, { initialLocale: 'en' }, createElement(App, { objectId: 'OBJ', sessionId: 'S', status: null })));
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

async function startContinue(extension = makeValidScanSnapshot()): Promise<any> {
  await act(async () => { expect(h.props.onScanIntent('continue')).toBe(true); h.props.onPointCloudReady(null); });
  await flush();
  await act(async () => { h.props.onPointCloudReady(extension); });
  await flush();
  const worker = h.workers[h.workers.length - 1];
  if (worker === undefined) throw new Error('Continuation did not create a worker.');
  await act(async () => { h.props.onTagRegistrationResult(null, true); });
  return worker;
}

beforeEach(() => {
  h.props = null; h.viewportProps = null; h.viewportEvents = []; h.workers = [];
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
  it('hands the tag-derived transform of a locked Continue to the worker as its registration seed, and none without a lock (issue #45)', async () => {
    const T = { yaw: 0.31, tx: 1.25, ty: -0.5, tz: 0.02 };
    await openSavedBase();
    await act(async () => { expect(h.props.onScanIntent('continue')).toBe(true); h.props.onPointCloudReady(null); });
    await flush();
    await act(async () => { h.props.onPointCloudReady(makeValidScanSnapshot(), { relockTransform: T }); });
    await flush();
    const locked = h.workers[h.workers.length - 1];
    expect(locked.request.options.seed).toEqual(T);

    await act(async () => { h.props.onScanIntent('fresh'); });
    h.workers = [];
    await act(async () => { expect(h.props.onScanIntent('continue')).toBe(true); h.props.onPointCloudReady(null); });
    await flush();
    await act(async () => { h.props.onPointCloudReady(makeValidScanSnapshot()); });
    await flush();
    const unlocked = h.workers[h.workers.length - 1];
    expect(unlocked.request.options).not.toHaveProperty('seed');
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
    expect(rendered).toContain('The scan continuation was not saved: worker failed');
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
