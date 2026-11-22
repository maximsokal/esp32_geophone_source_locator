import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createElement } from 'react';
import { act, create, type ReactTestRenderer } from 'react-test-renderer';

const h = vi.hoisted(() => ({
  viewport: null as any,
  loadScan: vi.fn(), loadTags: vi.fn(), loadReg: vi.fn(), fetchRuns: vi.fn(), loadPreview: vi.fn(), loadOccupancy: vi.fn(),
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
    loadSessionPreviewArtifact: h.loadPreview,
    loadSessionOccupancyArtifact: h.loadOccupancy,
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
import { encodeVoxelPreviewArtifact } from '../../src/xr/voxelPreview';
import { encodeOccupancySolverArtifact } from '../../src/xr/occupancySolverArtifact';
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
  h.loadPreview.mockReset().mockResolvedValue(null);
  h.loadOccupancy.mockReset().mockResolvedValue(null);
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

describe('ESP32 voxels in the embedded scan view (issue #44)', () => {
  const crc = Number.parseInt(SCAN_ID.slice('scan-crc32:'.length), 16);
  const preview = (sourceScanCrc32: number) => encodeVoxelPreviewArtifact({
    sourceScanCrc32, geometryComputedAtMs: 0n, registration: null, origin: [0, 0, 0], voxelSizeMeters: 0.25, dimensions: [2, 1, 1],
    voxelCount: 2, indexWidth: 2, indices: new Uint32Array([0, 1]), faceMasks: new Uint8Array([63, 63]), coarseningSteps: 0,
  });
  const occupancy = (frameId: string) => encodeOccupancySolverArtifact({
    freeAirPositions: new Float32Array([1.1, 0.1, 0.1]), occupiedSurfacePositions: new Float32Array([0.1, 0.1, 0.1]),
    occupancyEnvelopeBounds: null, occupancySolverCandidateBelowSurfaceMeters: 0, occupancyCellsSaturated: false,
    occupancyFineVoxelSizeMeters: 0.25, occupancyVoxelSizeMeters: 0.25,
  }, { frameId, createdAtMs: 1 });
  const choose = async (value: string) => {
    const select = renderer!.root.findByType('select');
    await act(async () => { select.props.onChange({ target: { value } }); });
    await flush();
  };

  it('offers the same representation select, shows the mesh first, and reads nothing from the device until a representation is chosen', async () => {
    await mount({ sessionId: null, finished: false });
    expect(renderer!.root.findAllByType('select')).toHaveLength(1);
    expect(h.viewport.esp32Voxels).toBeNull();
    expect(h.loadPreview).not.toHaveBeenCalled();
    expect(h.loadOccupancy).not.toHaveBeenCalled();
  });

  it('reads the TFT preview of the staged scan back from the ESP32, shows it as saved, and Mesh brings the scan back', async () => {
    h.loadPreview.mockResolvedValue(preview(crc));
    await mount({ sessionId: null, finished: false });
    await choose('esp32-preview');
    expect(h.loadPreview).toHaveBeenCalledWith('OBJ_0001', 'staging');
    expect(h.viewport.esp32Voxels).toMatchObject({ kind: 'preview', status: 'saved' });
    expect(h.viewport.esp32Voxels.artifact.voxelCount).toBe(2);
    await choose('mesh');
    expect(h.viewport.esp32Voxels).toBeNull();
  });

  it('reads the solver occupancy of a recording and shows it as saved', async () => {
    h.loadOccupancy.mockResolvedValue(occupancy(SCAN_ID));
    await mount({ sessionId: 'S000001', finished: true });
    await choose('esp32-solver');
    expect(h.loadOccupancy).toHaveBeenCalledWith('OBJ_0001', 'S000001');
    expect(h.viewport.esp32Voxels).toMatchObject({ kind: 'solver', status: 'saved' });
  });

  it('binds the device occupancy to the SHA-256 of the scan on screen: the same frame id with another digest is refused (audit 2026-11-22)', async () => {
    const SHA_A = 'a'.repeat(64); const SHA_B = 'b'.repeat(64);
    const bound = (sha: string) => encodeOccupancySolverArtifact({
      freeAirPositions: new Float32Array([1.1, 0.1, 0.1]), occupiedSurfacePositions: new Float32Array([0.1, 0.1, 0.1]),
      occupancyEnvelopeBounds: null, occupancySolverCandidateBelowSurfaceMeters: 0, occupancyCellsSaturated: false,
      occupancyFineVoxelSizeMeters: 0.25, occupancyVoxelSizeMeters: 0.25,
    }, { frameId: SCAN_ID, createdAtMs: 1, scanSha256: sha });
    h.decodeResult = { snapshot: { pointCount: 5 }, identity: { frameId: SCAN_ID, sha256: SHA_A } };
    h.loadOccupancy.mockResolvedValue(bound(SHA_B));
    await mount({ sessionId: 'S000001', finished: true });
    await choose('esp32-solver');
    expect(h.viewport.esp32Voxels).toBeNull();
    h.loadOccupancy.mockResolvedValue(bound(SHA_A));
    await choose('mesh');
    await choose('esp32-solver');
    expect(h.viewport.esp32Voxels).toMatchObject({ kind: 'solver', status: 'saved' });
  });

  it("draws nothing and says why when the device holds none, or holds another scan's", async () => {
    await mount({ sessionId: null, finished: false });
    await choose('esp32-preview');
    expect(h.viewport.esp32Voxels).toBeNull();
    expect(text()).toContain('holds no voxels for this scan');
    h.loadPreview.mockResolvedValue(preview(crc + 1));
    await choose('mesh');
    await choose('esp32-preview');
    expect(h.viewport.esp32Voxels).toBeNull();
    expect(text()).toContain('belong to another scan');
  });
});
