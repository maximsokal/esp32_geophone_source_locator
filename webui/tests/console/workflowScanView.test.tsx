import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createElement } from 'react';
import { act, create, type ReactTestRenderer } from 'react-test-renderer';

const h = vi.hoisted(() => ({
  viewport: null as any,
  loadScan: vi.fn(), loadTags: vi.fn(), loadReg: vi.fn(), fetchRuns: vi.fn(),
  decodeResult: null as any,
}));

vi.mock('../../src/api/deviceApi', async (importOriginal) => {
  const actual = await importOriginal<typeof import('../../src/api/deviceApi')>();
  return {
    ...actual,
    loadObjectScanArtifact: h.loadScan,
    loadSessionTagPositions: h.loadTags,
    loadSessionRegistration: h.loadReg,
    fetchLocalizationRuns: h.fetchRuns,
  };
});
vi.mock('../../src/xr/scanDecodeClient', async (importOriginal) => {
  const actual = await importOriginal<typeof import('../../src/xr/scanDecodeClient')>();
  return {
    ...actual,
    ScanDecodeClient: class {
      decode(): Promise<unknown> { return Promise.resolve(h.decodeResult); }
      dispose(): void { /* nothing to release */ }
    },
  };
});
vi.mock('../../src/components/ScanPointCloudViewport', () => ({
  ScanPointCloudViewport: (props: unknown) => { h.viewport = props; return null; },
}));
vi.mock('../../src/i18n', async (importOriginal) => {
  const actual = await importOriginal<typeof import('../../src/i18n')>();
  return {
    ...actual,
    useT: () => (key: Parameters<typeof actual.translate>[1], params?: Parameters<typeof actual.translate>[2]) => actual.translate('en', key, params),
  };
});

import WorkflowScanView from '../../src/features/workflow/WorkflowScanView';
import type { TagSensorAnchor } from '../../src/xr/fiducial/liveTagTracker';
import { buildTagLayout } from '../../src/xr/fiducial/tagLayout';
import { encodeTagPositions, tagPositionsFromLayout } from '../../src/xr/fiducial/tagPositionsArtifact';

const SCAN_ID = 'scan-crc32:732740d9';
const anchor = (tagId: number, node: string, xr: [number, number, number]): TagSensorAnchor =>
  ({ tagId, node, positionScan: xr, spreadM: 0.003, observationCount: 8, independentViewCount: 4, confirmed: true });
const layout = buildTagLayout([anchor(0, 'MAIN', [1, 1, -1]), anchor(1, 'G1', [1.5, 1, -1]), anchor(2, 'G2', [1, 1, -1.8]), anchor(3, 'G3', [0.9, 1.2, -1.5])]).layout!;
const tagsJson = (scanId: string | null = SCAN_ID) => encodeTagPositions(tagPositionsFromLayout(layout, scanId, 1_700_000_000_000));

const run = (overrides: Record<string, unknown> = {}) => ({
  kind: 'individual_method', method_name: 'm', method_version: 1, object_id: 'OBJ_0001', session_id: 'S000001',
  computed_at_ms: 10, geometry_computed_at_ms: 4242, solved: true, velocity_status: 'consistent',
  source_position: { x_m: 0.5, y_m: 0.25, z_m: -0.1 }, ...overrides,
});
const listing = (source: string, runs: unknown[] = [run()]) => ({
  object_id: 'OBJ_0001', session_id: 'S000001', runs, picks: [], partial_error: false, partial_errors: [],
  geometry: { available: true, pods: [{ pod: 1, computed: true, position_source: source }, { pod: 2, computed: true, position_source: source }] },
});

let renderer: ReactTestRenderer | null = null;
const flush = async () => { for (let i = 0; i < 8; i += 1) await act(async () => { await Promise.resolve(); }); };
const text = () => JSON.stringify(renderer?.toJSON() ?? null);
async function mount(props: { sessionId: string | null; finished: boolean }) {
  await act(async () => { renderer = create(createElement(WorkflowScanView, { objectId: 'OBJ_0001', ...props })); });
  await flush();
}

beforeEach(() => {
  h.viewport = null;
  h.loadScan.mockReset().mockResolvedValue(new Uint8Array([1]));
  h.loadTags.mockReset().mockResolvedValue(tagsJson());
  h.loadReg.mockReset().mockResolvedValue(null);
  h.fetchRuns.mockReset().mockResolvedValue(listing('apriltag_layout'));
  h.decodeResult = { snapshot: { pointCount: 5 }, identity: { frameId: SCAN_ID } };
});
afterEach(() => { act(() => { renderer?.unmount(); }); renderer = null; });

describe('3D scan on the recording page', () => {
  it('before the recording exists it reads the staged scan, shows MAIN and the geophones where the tags put them, and promises the source', async () => {
    await mount({ sessionId: null, finished: false });
    expect(h.loadScan).toHaveBeenCalledWith('OBJ_0001', 'staging');
    expect(h.loadTags).toHaveBeenCalledWith('OBJ_0001', 'staging');
    expect(h.fetchRuns).not.toHaveBeenCalled();                       // nothing is calculated yet
    expect(h.viewport.sensorMarkers.map((marker: any) => marker.node)).toEqual(['MAIN', 'G1', 'G2', 'G3']);
    expect(h.viewport.sensorMarkers[1].positionScanM).toEqual([1.5, 1, 1]);
    expect(h.viewport.registrationArtifact).toBeNull();
    expect(text()).toContain('Devices placed from AprilTags: 4 of 5');
    expect(text()).toContain('The source will be marked after the calculation.');
  });

  it('after the calculation it places the source with the translation to MAIN when the calculation used the tag positions', async () => {
    await mount({ sessionId: 'S000001', finished: true });
    expect(h.loadScan).toHaveBeenCalledWith('OBJ_0001', 'S000001');
    expect(h.viewport.localizationRuns).toHaveLength(1);
    expect(h.viewport.currentScanId).toBe(SCAN_ID);
    const registration = h.viewport.registrationArtifact;
    expect(registration).toMatchObject({ valid: true, scan_frame_id: SCAN_ID, geometry_computed_at_ms: 4242 });
    expect(registration.sensor_to_scan[3]).toBeCloseTo(1, 9);         // MAIN's scan x (m)
    expect(text()).toContain('The source of the seismic event is marked in the scan.');
  });

  it('does not invent a transform for a Channel Sounding result without a registration that applies to this scan', async () => {
    h.fetchRuns.mockResolvedValue(listing('channel_sounding'));
    await mount({ sessionId: 'S000001', finished: true });
    expect(h.viewport.registrationArtifact).toBeNull();
    expect(text()).toContain('cannot be placed in this scan');
  });

  it('ignores tag positions that were saved for another scan', async () => {
    h.loadTags.mockResolvedValue(tagsJson('scan-crc32:00000000'));
    await mount({ sessionId: null, finished: false });
    expect(h.viewport.sensorMarkers).toEqual([]);
    expect(text()).toContain('No AprilTag positions were saved with this scan');
  });

  it('shows nothing at all when there is no scan, and says so when the scan cannot be shown', async () => {
    h.loadScan.mockResolvedValue(null);
    await mount({ sessionId: null, finished: false });
    expect(renderer?.toJSON()).toBeNull();
    act(() => { renderer?.unmount(); }); renderer = null;
    h.loadScan.mockRejectedValue(new Error('boom'));
    await mount({ sessionId: null, finished: false });
    expect(text()).toContain('The 3D scan could not be shown.');
  });
});
