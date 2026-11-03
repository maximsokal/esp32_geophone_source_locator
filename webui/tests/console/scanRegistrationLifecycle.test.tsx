import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createElement } from 'react';
import { act, create, type ReactTestRenderer } from 'react-test-renderer';

const h = vi.hoisted(() => ({
  props: null as any,
  viewportProps: null as any,
  useRealScanLoad: false,
  fetchRuns: vi.fn(), loadScan: vi.fn(), loadReg: vi.fn(), saveReg: vi.fn(), delReg: vi.fn(),
  saveScan: vi.fn(), saveOcc: vi.fn(), savePrev: vi.fn(), info: vi.fn(), delScan: vi.fn(),
}));

vi.mock('../../src/api/deviceApi', async (importOriginal) => {
  const actual = await importOriginal<typeof import('../../src/api/deviceApi')>();
  return {
    ...actual,
    fetchLocalizationRuns: h.fetchRuns,
    loadObjectScanArtifact: (...args: Parameters<typeof actual.loadObjectScanArtifact>) =>
      h.useRealScanLoad ? actual.loadObjectScanArtifact(...args) : h.loadScan(...args),
    loadSessionRegistration: h.loadReg,
    saveSessionRegistration: h.saveReg, deleteSessionRegistration: h.delReg, saveObjectScanArtifact: h.saveScan,
    saveSessionOccupancyArtifact: h.saveOcc, saveSessionPreviewArtifact: h.savePrev, fetchScanInfo: h.info,
    deleteObjectScanArtifact: h.delScan,
  };
});
vi.mock('../../src/components/XrRuntimePanel', () => ({ XrRuntimePanel: (props: unknown) => { h.props = props; return null; } }));
vi.mock('../../src/components/ScanPointCloudViewport', () => ({ ScanPointCloudViewport: (props: unknown) => { h.viewportProps = props; return null; } }));
vi.mock('../../src/xr/deviceComputeProfile', () => ({ deviceComputeProfilePromise: new Promise(() => undefined) }));

import App from '../../src/App';
import { LocaleProvider, translate } from '../../src/i18n';
import { decodeScanArtifact, encodeScanArtifact } from '../../src/xr/scanArtifact';
import { rewriteScanArtifactMetadata } from '../xr/support/scanArtifactTestTools';
import { decodeOccupancySolverArtifact, occupancyFrameIdForScan } from '../../src/xr/occupancySolverArtifact';
import { decodeVoxelPreviewArtifact } from '../../src/xr/voxelPreview';
import {
  decodeRegistrationArtifact, defaultRegistrationTagSizes, encodeRegistrationArtifact, type RegistrationArtifact,
} from '../../src/xr/fiducial/registrationArtifact';
import { makeValidScanSnapshot } from '../xr/support/validScanSnapshot';

const GEOMETRY_MS = 500;
const mounted: ReactTestRenderer[] = [];

function registration(patch: Partial<RegistrationArtifact> = {}): RegistrationArtifact {
  return {
    schema: 1, frame: 'webxr_local_floor', scan_frame_id: null, source: 'apriltag', family: 'tag36h11',
    geometry_computed_at_ms: GEOMETRY_MS, created_at_ms: 600, tag_size_m: defaultRegistrationTagSizes(), bindings: [],
    sensor_to_scan: [0, 0, 1, 0.5, 1, 0, 0, 0.25, 0, 1, 0, -1.5, 0, 0, 0, 1], mirrored: false,
    residual_rms_m: 0.01, threshold_m: 0.05, per_node: [], tier: 'minimum', valid: true, reason: null, ...patch,
  };
}

function scanBytes(): Uint8Array { return encodeScanArtifact(makeValidScanSnapshot()); }
function scanIdOf(bytes: Uint8Array): string { return occupancyFrameIdForScan(bytes); }

async function flush(): Promise<void> {
  await act(async () => { for (let i = 0; i < 200; i += 1) await Promise.resolve(); });
}

async function mountApp(): Promise<ReactTestRenderer> {
  let renderer!: ReactTestRenderer;
  await act(async () => {
    renderer = create(createElement(LocaleProvider, { initialLocale: 'en' }, createElement(App, { objectId: 'OBJ', sessionId: 'S', status: null })));
  });
  mounted.push(renderer);
  await flush();
  return renderer;
}

/** The committed device state used by the server stubs. */
const device = { scan: null as Uint8Array | null, occupancy: null as Uint8Array | null, preview: null as Uint8Array | null };

beforeEach(() => {
  h.props = null;
  h.viewportProps = null;
  h.useRealScanLoad = false;
  device.scan = null; device.occupancy = null; device.preview = null;
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  vi.stubGlobal('window', { localStorage: { getItem: () => null, setItem: () => undefined }, addEventListener: () => undefined, removeEventListener: () => undefined, isSecureContext: false, location: { href: 'http://localhost/' }, setTimeout: globalThis.setTimeout.bind(globalThis), clearTimeout: globalThis.clearTimeout.bind(globalThis) });
  vi.stubGlobal('document', { hidden: false, visibilityState: 'visible', addEventListener: () => undefined, removeEventListener: () => undefined });
  vi.stubGlobal('navigator', {});
  h.fetchRuns.mockReset().mockResolvedValue({ object_id: 'OBJ', session_id: 'S', runs: [], geometry: { available: true, pods: [{ pod: 1, computed: true, leveled: true, computed_at_ms: GEOMETRY_MS }] } });
  h.loadScan.mockReset().mockImplementation(async () => device.scan);
  h.loadReg.mockReset().mockResolvedValue(null);
  h.saveReg.mockReset().mockResolvedValue({ ok: true, action: 'registration' });
  h.delReg.mockReset().mockResolvedValue({ ok: true, action: 'delete' });
  h.delScan.mockReset().mockResolvedValue({ ok: true, action: 'delete' });
  h.saveScan.mockReset().mockImplementation(async (_o: string, _s: string, bytes: Uint8Array) => {
    device.scan = bytes.slice(); device.occupancy = null; device.preview = null;
    return { ok: true, action: 'scan', id: scanIdOf(bytes) };
  });
  h.saveOcc.mockReset().mockImplementation(async (_o: string, _s: string, bytes: Uint8Array) => { device.occupancy = bytes.slice(); return { ok: true, action: 'occupancy' }; });
  h.savePrev.mockReset().mockImplementation(async (_o: string, _s: string, bytes: Uint8Array) => { device.preview = bytes.slice(); return { ok: true, action: 'preview' }; });
  h.info.mockReset().mockImplementation(async (_o: string, _s: string, kind: 'scan' | 'occupancy' | 'preview') => {
    const bytes = kind === 'scan' ? device.scan : kind === 'occupancy' ? device.occupancy : device.preview;
    if (bytes === null) return { present: false, bytes: 0 };
    const scanId = device.scan === null ? null : scanIdOf(device.scan);
    return kind === 'scan' ? { present: true, bytes: bytes.byteLength, id: scanId } : { present: true, bytes: bytes.byteLength, id: null, sourceId: scanId };
  });
});

afterEach(() => {
  for (const renderer of [...mounted]) { act(() => renderer.unmount()); mounted.splice(mounted.indexOf(renderer), 1); }
  vi.unstubAllGlobals();
});

/** A different scan (new generation) than scanBytes(). */
function variantSnapshot() {
  const point: readonly [number, number, number] = [0.5, 0.2, 0.3];
  return { ...makeValidScanSnapshot(), positions: new Float32Array(point), bounds: { min: point, max: point }, fullBounds: { min: point, max: point } };
}

async function retryOccupancy(renderer: ReactTestRenderer): Promise<void> {
  const label = translate('en', 'scan.retry.occupancy');
  const button = renderer.root.findAllByType('button').find((candidate) => candidate.children.join('') === label);
  if (button === undefined) throw new Error('Missing occupancy retry button.');
  await act(async () => { button.props.onClick(); });
  await flush();
}

/** Drives the production App through one AR session the way XrRuntimePanel does. */
async function finishScan(intent: 'fresh' | 'continue', tag: RegistrationArtifact | null, options: { tagFirst?: boolean } = {}): Promise<void> {
  await act(async () => { h.props.onScanIntent(intent); h.props.onPointCloudReady(null); });
  await act(async () => { h.props.onPointCloudReady(intent === 'fresh' ? variantSnapshot() : makeValidScanSnapshot()); });
  if (options.tagFirst !== true) await act(async () => { h.props.onTagRegistrationResult(tag, true); });
  await flush();
}

function lastCall<T>(mock: { mock: { calls: T[][] } }): T[] {
  return mock.mock.calls[mock.mock.calls.length - 1];
}

describe('App scan / registration lifecycle (MAX-405, MAX-403)', () => {
  it('binds a Fresh tag registration to the new scan, embeds it in occupancy and preview and persists it once after the commit', async () => {
    await mountApp();
    await finishScan('fresh', registration());
    const id = scanIdOf(device.scan!);
    const occupancy = decodeOccupancySolverArtifact(device.occupancy!);
    expect(occupancy.header.frame_id).toBe(id);
    expect(occupancy.header.registration).toMatchObject({ frame: 'project_z_up', geometry_computed_at_ms: GEOMETRY_MS, threshold_m: 0.05, residual_m: 0.01 });
    expect(decodeVoxelPreviewArtifact(device.preview!).registration).not.toBeNull();
    expect(h.saveReg).toHaveBeenCalledTimes(1);
    const saved = decodeRegistrationArtifact(lastCall(h.saveReg)[2] as string);
    expect(saved.scan_frame_id).toBe(id);
    expect(h.saveScan).toHaveBeenCalledTimes(1);
    expect(h.saveOcc).toHaveBeenCalledTimes(1);
    expect(h.savePrev).toHaveBeenCalledTimes(1);
  });

  it('starts the full-scan upload and the derivative uploads even while the registration save is held open', async () => {
    let release!: (value: unknown) => void;
    h.saveReg.mockImplementation(() => new Promise((resolve) => { release = resolve; }));
    await mountApp();
    await finishScan('fresh', registration());
    expect(h.saveScan).toHaveBeenCalledTimes(1);
    expect(h.saveOcc).toHaveBeenCalledTimes(1);
    expect(h.savePrev).toHaveBeenCalledTimes(1);
    expect(h.saveReg).toHaveBeenCalledTimes(1);
    release({ ok: true, action: 'registration' });
    await flush();
  });

  it('uploads the scan before the tag result exists and leaves derivatives unregistered when there is none', async () => {
    await mountApp();
    await finishScan('fresh', null, { tagFirst: true });
    expect(h.saveScan).toHaveBeenCalledTimes(1);
    expect(h.saveOcc).not.toHaveBeenCalled();
    await act(async () => { h.props.onTagRegistrationResult(null, true); });
    await flush();
    expect(decodeOccupancySolverArtifact(device.occupancy!).header.registration).toBeNull();
    expect(h.saveReg).not.toHaveBeenCalled();
  });

  it('reports a failed registration stage explicitly while the saved scan survives', async () => {
    h.saveReg.mockResolvedValue({ ok: false, action: 'registration', error: 'disk full' });
    const renderer = await mountApp();
    await finishScan('fresh', registration());
    expect(device.scan).not.toBeNull();
    const text = JSON.stringify(renderer.toJSON());
    expect(text).toContain('Not saved (the scan itself is saved)');
  });
});

describe('App scan restore validation (MAX-408)', () => {
  it('rejects a CRC-valid semantic failure before viewport reconciliation and preserves device bytes', async () => {
    const goodDeviceBytes = scanBytes();
    device.scan = goodDeviceBytes.slice();
    const malformed = rewriteScanArtifactMetadata(encodeScanArtifact(makeValidScanSnapshot()), (metadata) => {
      metadata.pointCount = 2;
    });
    h.useRealScanLoad = true;
    const fetchMock = vi.fn(async (url: string) => {
      if (!url.includes('scan=load')) throw new Error(`Unexpected HTTP request: ${url}`);
      return new Response(malformed.slice().buffer, { status: 200, headers: { 'Content-Type': 'application/octet-stream' } });
    });
    vi.stubGlobal('fetch', fetchMock);

    const renderer = await mountApp();
    await flush();

    expect(fetchMock).toHaveBeenCalledTimes(1);
    expect(fetchMock.mock.calls[0][0]).toContain('scan=load');
    expect(h.viewportProps).toBeNull();
    expect(h.info).not.toHaveBeenCalled();
    expect(h.saveOcc).not.toHaveBeenCalled();
    expect(h.savePrev).not.toHaveBeenCalled();
    expect(h.delScan).not.toHaveBeenCalled();
    expect(device.scan).toEqual(goodDeviceBytes);
    expect(JSON.stringify(renderer.toJSON())).toContain(translate('en', 'scan.error.restore_failed'));
  });
});

describe('App registration ownership across scans (MAX-405, MAX-416)', () => {
  /** A durable scan A with a registration bound to it, as after a reload. */
  async function openSavedScanWithRegistration(): Promise<{ aId: string }> {
    device.scan = scanBytes();
    const aId = scanIdOf(device.scan);
    device.occupancy = new Uint8Array([1]);
    device.preview = new Uint8Array([1]);
    h.loadReg.mockResolvedValue(encodeRegistrationArtifact(registration({ scan_frame_id: aId })));
    await mountApp();
    return { aId };
  }

  it('does not embed or keep the old registration when the same page scans again (Fresh)', async () => {
    await openSavedScanWithRegistration();
    await finishScan('fresh', null);
    expect(decodeOccupancySolverArtifact(device.occupancy!).header.registration).toBeNull();
    expect(decodeVoxelPreviewArtifact(device.preview!).registration).toBeNull();
    expect(h.saveReg).not.toHaveBeenCalled();
    expect(h.delReg).toHaveBeenCalledTimes(1);
  });

  it('does not apply a deleted scan registration to the next Fresh scan', async () => {
    await openSavedScanWithRegistration();
    await act(async () => { h.props.onDeleteSavedScan(); });
    await flush();
    expect(h.delScan).toHaveBeenCalledTimes(1);
    device.scan = null; device.occupancy = null; device.preview = null;
    await finishScan('fresh', null);
    expect(decodeOccupancySolverArtifact(device.occupancy!).header.registration).toBeNull();
  });

  it('restores an applicable registration after reload so a rebuilt occupancy carries it', async () => {
    device.scan = scanBytes();
    const aId = scanIdOf(device.scan);
    h.loadReg.mockResolvedValue(encodeRegistrationArtifact(registration({ scan_frame_id: aId })));
    await retryOccupancy(await mountApp());
    expect(h.saveOcc).toHaveBeenCalledTimes(1);
    expect(decodeOccupancySolverArtifact(device.occupancy!).header.registration).toMatchObject({ frame: 'project_z_up', geometry_computed_at_ms: GEOMETRY_MS });
  });

  it('refuses a restored registration bound to another scan or geometry when rebuilding derivatives', async () => {
    for (const patch of [{ scan_frame_id: 'scan-crc32:deadbeef' }, { scan_frame_id: null }, { geometry_computed_at_ms: GEOMETRY_MS + 1 }]) {
      device.scan = scanBytes(); device.occupancy = null; device.preview = null;
      h.saveOcc.mockClear();
      h.loadReg.mockResolvedValue(encodeRegistrationArtifact(registration({ scan_frame_id: scanIdOf(device.scan), ...patch })));
      const renderer = await mountApp();
      await retryOccupancy(renderer);
      expect(decodeOccupancySolverArtifact(device.occupancy!).header.registration).toBeNull();
      act(() => renderer.unmount());
      mounted.splice(mounted.indexOf(renderer), 1);
    }
  });

  it('changes nothing durable when a continuation is rejected', async () => {
    const { aId } = await openSavedScanWithRegistration();
    const scanBefore = device.scan!.slice();
    await finishScan('continue', registration());
    expect(h.saveScan).not.toHaveBeenCalled();
    expect(h.saveReg).not.toHaveBeenCalled();
    expect(h.delReg).not.toHaveBeenCalled();
    expect(Array.from(device.scan!)).toEqual(Array.from(scanBefore));
    expect(scanIdOf(device.scan!)).toBe(aId);
    expect(decodeScanArtifact(device.scan!).pointCount).toBe(1);
  });
});

describe('App scan information: distances recalculated from the scan', () => {
  const pods = [
    { pod: 0, node_id: 0, computed: true, assigned: true, leveled: true, computed_at_ms: GEOMETRY_MS },
    { pod: 1, node_id: 1, computed: true, assigned: true, leveled: true, x_mm: 3000, y_mm: 0, z_mm: 0, computed_at_ms: GEOMETRY_MS },
    { pod: 2, node_id: 2, computed: true, assigned: true, leveled: true, x_mm: 0, y_mm: 4000, z_mm: 0, computed_at_ms: GEOMETRY_MS },
  ];
  const tag = (node: 'MAIN' | 'G1' | 'G2', tagId: number, position: [number, number, number]) =>
    ({ tag_id: tagId, node, observation_count: 9, inlier_count: 8, position_scan_m: position, spread_m: 0.01 });
  const scannedDevices = [tag('MAIN', 0, [0, 0, 0]), tag('G1', 1, [2.8, 0, 0]), tag('G2', 2, [0, 4.1, 0])];

  function card(renderer: ReactTestRenderer) {
    return renderer.root.findAll((node) => node.props['data-refinement-status'] !== undefined && typeof node.type === 'string');
  }
  const textOf = (node: { children: unknown[] }): string => node.children.map((child) =>
    typeof child === 'string' ? child : textOf(child as { children: unknown[] })).join('');

  beforeEach(() => {
    h.fetchRuns.mockReset().mockResolvedValue({ object_id: 'OBJ', session_id: 'S', runs: [], geometry: { available: true, pods } });
  });

  it('shows the scanned devices, the recalculated pairs and the error reduction once the scan with tags has finished', async () => {
    const renderer = await mountApp();
    await finishScan('fresh', registration({ bindings: scannedDevices }));
    const cards = card(renderer);
    expect(cards).toHaveLength(1);
    expect(cards[0].props['data-refinement-status']).toBe('ok');
    const text = textOf(cards[0]);
    expect(text).toContain(translate('en', 'scan.refine.title'));
    expect(text).toContain('MAIN, G1, G2');
    for (const pair of ['MAIN–G1', 'MAIN–G2', 'G1–G2']) expect(text).toContain(pair);
    const percent = renderer.root.findAll((node) => node.props['data-reduction-percent'] !== undefined)[0];
    expect(Number(percent.props['data-reduction-percent'])).toBeGreaterThan(60);
    expect(Number(percent.props['data-reduction-percent'])).toBeLessThan(75);
    /* The scanned devices are also placed in the 3D scan canvas: one marker per tag, at a scan position. */
    const markers = (h.viewportProps as { sensorMarkers: { node: string; tagId: number | null; positionScanM: unknown; source: string | null }[] }).sensorMarkers;
    const byNode = Object.fromEntries(markers.map((marker) => [marker.node, marker]));
    for (const [node, tagId] of [['MAIN', 0], ['G1', 1], ['G2', 2]] as const) {
      expect(byNode[node]).toMatchObject({ tagId, source: 'apriltag' });
      expect(byNode[node].positionScanM).not.toBeNull();
    }
  });

  it('explains that no device was scanned when the scan has no tag result', async () => {
    const renderer = await mountApp();
    await finishScan('fresh', null);
    const cards = card(renderer);
    expect(cards).toHaveLength(1);
    expect(cards[0].props['data-refinement-status']).toBe('unavailable');
    expect(textOf(cards[0])).toContain(translate('en', 'scan.refine.unavailable.no_registration'));
  });

  it('is shown again for a saved scan after a reload, from its saved registration', async () => {
    device.scan = scanBytes();
    const id = scanIdOf(device.scan);
    device.occupancy = new Uint8Array([1]);
    device.preview = new Uint8Array([1]);
    h.loadReg.mockResolvedValue(encodeRegistrationArtifact(registration({ scan_frame_id: id, bindings: scannedDevices })));
    const renderer = await mountApp();
    const cards = card(renderer);
    expect(cards).toHaveLength(1);
    expect(cards[0].props['data-refinement-status']).toBe('ok');
  });

  it('never shows distances from a registration that belongs to another scan', async () => {
    device.scan = scanBytes();
    device.occupancy = new Uint8Array([1]);
    device.preview = new Uint8Array([1]);
    h.loadReg.mockResolvedValue(encodeRegistrationArtifact(registration({ scan_frame_id: 'scan-crc32:deadbeef', bindings: scannedDevices })));
    const renderer = await mountApp();
    const cards = card(renderer);
    expect(cards).toHaveLength(1);
    expect(cards[0].props['data-refinement-status']).toBe('unavailable');
  });
});
