import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { createElement } from 'react';
import { act, create } from 'react-test-renderer';
import { describe, expect, it, vi } from 'vitest';

import { clientDiagnostics } from '../../src/data/clientDiagnostics';
import { loadSavedEsp32Voxels, useSavedEsp32Voxels, type SavedVoxelLoaders, type SavedVoxelRequest, type SavedVoxelState } from '../../src/xr/esp32SavedVoxels';
import { encodeOccupancySolverArtifact } from '../../src/xr/occupancySolverArtifact';
import { encodeVoxelPreviewArtifact } from '../../src/xr/voxelPreview';

const SCAN_ID = 'scan-crc32:00000007';
const SHA = 'a'.repeat(64);

const previewBytes = (crc: number): Uint8Array => encodeVoxelPreviewArtifact({
  sourceScanCrc32: crc, geometryComputedAtMs: 0n, registration: null, origin: [0, 0, 0], voxelSizeMeters: 0.25, dimensions: [2, 1, 1],
  voxelCount: 2, indexWidth: 2, indices: new Uint32Array([0, 1]), faceMasks: new Uint8Array([63, 63]), coarseningSteps: 0,
});
const occupancyBytes = (frameId: string, scanSha256?: string): Uint8Array => encodeOccupancySolverArtifact({
  freeAirPositions: new Float32Array([1.1, 0.1, 0.1]),
  occupiedSurfacePositions: new Float32Array([0.1, 0.1, 0.1, 0.6, 0.1, 0.1]),
  occupancyEnvelopeBounds: null, occupancySolverCandidateBelowSurfaceMeters: 0, occupancyCellsSaturated: false,
  occupancyFineVoxelSizeMeters: 0.25, occupancyVoxelSizeMeters: 0.25,
}, { frameId, createdAtMs: 1, ...(scanSha256 === undefined ? {} : { scanSha256 }) });

const loaders = (preview: Uint8Array | null, occupancy: Uint8Array | null): SavedVoxelLoaders => ({
  preview: vi.fn(async () => preview),
  occupancy: vi.fn(async () => occupancy),
});
const request = (kind: 'preview' | 'solver', extra: Partial<SavedVoxelRequest> = {}): SavedVoxelRequest =>
  ({ objectId: 'OBJ_0001', sessionId: 'staging', kind, scanId: SCAN_ID, ...extra });

describe('voxels read back from the ESP32 (issue #44)', () => {
  it('reports a derivative the device does not hold as missing and draws nothing', async () => {
    expect(await loadSavedEsp32Voxels(request('preview'), loaders(null, null))).toEqual({ state: 'missing' });
    expect(await loadSavedEsp32Voxels(request('solver'), loaders(null, null))).toEqual({ state: 'missing' });
  });

  it('decodes the preview of THIS scan and calls it saved, asking the device for exactly this object and session', async () => {
    const source = loaders(previewBytes(7), null);
    const result = await loadSavedEsp32Voxels(request('preview'), source);
    expect(result.state).toBe('ready');
    if (result.state === 'ready' && result.view.kind === 'preview') {
      expect(result.view.status).toBe('saved');
      expect(result.view.artifact.voxelCount).toBe(2);
    } else throw new Error('expected a preview view');
    expect(source.preview).toHaveBeenCalledWith('OBJ_0001', 'staging');
    expect(source.occupancy).not.toHaveBeenCalled();
  });

  it('refuses a preview of another scan (CRC32 differs) as stale', async () => {
    expect(await loadSavedEsp32Voxels(request('preview'), loaders(previewBytes(8), null))).toEqual({ state: 'stale' });
    expect(await loadSavedEsp32Voxels(request('preview', { scanId: 'not-an-identity' }), loaders(previewBytes(7), null))).toEqual({ state: 'stale' });
  });

  it('TASK 11: every load is one bounded SCAN_VOXEL_LOAD_RESULT, and a rejected identity names the check that failed and both values (SCAN_VOXEL_IDENTITY_MISMATCH)', async () => {
    const info = vi.spyOn(clientDiagnostics, 'info').mockImplementation(() => undefined);
    const warn = vi.spyOn(clientDiagnostics, 'warn').mockImplementation(() => undefined);
    try {
      await loadSavedEsp32Voxels(request('preview'), loaders(previewBytes(8), null));
      expect(warn).toHaveBeenCalledWith('SCAN_VOXEL_IDENTITY_MISMATCH', { objectId: 'OBJ_0001', sessionId: 'staging', kind: 'preview', mismatch: 'crc32', expected: '00000007', found: '00000008' });
      await loadSavedEsp32Voxels(request('solver'), loaders(null, occupancyBytes('scan-crc32:00000009')));
      expect(warn).toHaveBeenCalledWith('SCAN_VOXEL_IDENTITY_MISMATCH', expect.objectContaining({ kind: 'solver', mismatch: 'frame_id', expected: SCAN_ID, found: 'scan-crc32:00000009' }));
      await loadSavedEsp32Voxels(request('solver', { scanSha256: SHA }), loaders(null, occupancyBytes(SCAN_ID, 'b'.repeat(64))));
      expect(warn).toHaveBeenCalledWith('SCAN_VOXEL_IDENTITY_MISMATCH', expect.objectContaining({ mismatch: 'sha256', expected: 'a'.repeat(16), found: 'b'.repeat(16) }));
      await loadSavedEsp32Voxels(request('preview', { scanId: 'not-an-identity' }), loaders(previewBytes(7), null));
      expect(warn).toHaveBeenCalledWith('SCAN_VOXEL_IDENTITY_MISMATCH', expect.objectContaining({ mismatch: 'unparsable_scan_id' }));
      info.mockClear();
      await loadSavedEsp32Voxels(request('preview'), loaders(previewBytes(7), null));
      await loadSavedEsp32Voxels(request('solver'), loaders(null, null));
      expect(info).toHaveBeenCalledTimes(2);
      expect(info).toHaveBeenNthCalledWith(1, 'SCAN_VOXEL_LOAD_RESULT', expect.objectContaining({ kind: 'preview', scan: SCAN_ID, state: 'ready' }));
      expect(info).toHaveBeenNthCalledWith(2, 'SCAN_VOXEL_LOAD_RESULT', expect.objectContaining({ kind: 'solver', state: 'missing', bytes: 0 }));
    } finally { info.mockRestore(); warn.mockRestore(); }
  });

  it('decodes the solver occupancy of THIS scan: frame id, and the SHA-256 when both sides have one', async () => {
    const ok = await loadSavedEsp32Voxels(request('solver', { scanSha256: SHA }), loaders(null, occupancyBytes(SCAN_ID, SHA)));
    expect(ok.state).toBe('ready');
    if (ok.state === 'ready' && ok.view.kind === 'solver') {
      expect(ok.view.status).toBe('saved');
      expect(ok.view.occupancy.header.occupied_count).toBe(2);
      expect(ok.view.occupancy.header.free_count).toBe(1);
    } else throw new Error('expected a solver view');
    /* An older artifact without a SHA is bound by the frame id alone. */
    expect((await loadSavedEsp32Voxels(request('solver', { scanSha256: SHA }), loaders(null, occupancyBytes(SCAN_ID)))).state).toBe('ready');
  });

  it('refuses an occupancy of another scan: another frame id, or the same CRC32 with another SHA-256 (colliding scans)', async () => {
    expect(await loadSavedEsp32Voxels(request('solver'), loaders(null, occupancyBytes('scan-crc32:00000009')))).toEqual({ state: 'stale' });
    expect(await loadSavedEsp32Voxels(request('solver', { scanSha256: SHA }), loaders(null, occupancyBytes(SCAN_ID, 'b'.repeat(64))))).toEqual({ state: 'stale' });
  });

  it('reports unreadable bytes as invalid, with the reason, instead of drawing them', async () => {
    const corruptPreview = previewBytes(7).slice();
    corruptPreview[corruptPreview.length - 1] ^= 0xff;
    const preview = await loadSavedEsp32Voxels(request('preview'), loaders(corruptPreview, null));
    expect(preview.state).toBe('invalid');
    const corruptOccupancy = occupancyBytes(SCAN_ID).slice();
    corruptOccupancy[corruptOccupancy.length - 1] ^= 0xff;
    expect((await loadSavedEsp32Voxels(request('solver'), loaders(null, corruptOccupancy))).state).toBe('invalid');
  });
});

function harness(initial: SavedVoxelRequest | null, source: SavedVoxelLoaders) {
  let latest: SavedVoxelState | null = null;
  const Probe = ({ value }: { value: SavedVoxelRequest | null }) => { latest = useSavedEsp32Voxels(value, source); return null; };
  let renderer!: ReturnType<typeof create>;
  const mount = async (value: SavedVoxelRequest | null) => {
    await act(async () => { renderer = create(createElement(Probe, { value })); });
  };
  const update = async (value: SavedVoxelRequest | null) => {
    await act(async () => { renderer.update(createElement(Probe, { value })); });
  };
  const flush = async () => { for (let i = 0; i < 6; i += 1) await act(async () => { await Promise.resolve(); }); };
  return { mount: () => mount(initial), update, flush, state: () => latest!, unmount: () => act(() => renderer.unmount()) };
}

describe('useSavedEsp32Voxels', () => {
  it('stays idle without a request, loads when one appears, and drops the result when it goes away', async () => {
    let release!: (bytes: Uint8Array | null) => void;
    const held = new Promise<Uint8Array | null>((resolve) => { release = resolve; });
    const source: SavedVoxelLoaders = { preview: vi.fn(() => held), occupancy: vi.fn(async () => null) };
    const view = harness(null, source);
    await view.mount();
    expect(view.state().phase).toBe('idle');
    expect(source.preview).not.toHaveBeenCalled();
    await view.update(request('preview'));
    expect(view.state().phase).toBe('loading');          /* the read is still in flight */
    release(previewBytes(7));
    await view.flush();
    expect(view.state().phase).toBe('ready');
    expect(view.state().view?.kind).toBe('preview');
    await view.update(null);
    expect(view.state()).toEqual({ phase: 'idle', view: null, error: null });
    view.unmount();
  });

  it('ignores the late answer of an older request: voxels of another scan never reach the viewer', async () => {
    let releaseOld!: (bytes: Uint8Array | null) => void;
    const old = new Promise<Uint8Array | null>((resolve) => { releaseOld = resolve; });
    const source: SavedVoxelLoaders = {
      preview: vi.fn((_object: string, session: string) => (session === 'S000001' ? old : Promise.resolve(previewBytes(7)))),
      occupancy: vi.fn(async () => null),
    };
    const view = harness(request('preview', { sessionId: 'S000001' }), source);
    await view.mount();
    await view.update(request('preview', { sessionId: 'staging' }));
    await view.flush();
    expect(view.state().phase).toBe('ready');
    releaseOld(previewBytes(8));
    await view.flush();
    expect(view.state().phase).toBe('ready');
    expect(view.state().view?.kind === 'preview' && view.state().view?.status === 'saved').toBe(true);
    view.unmount();
  });

  it('reports a failed read instead of staying in loading', async () => {
    const source: SavedVoxelLoaders = { preview: vi.fn(async () => { throw new Error('network down'); }), occupancy: vi.fn(async () => null) };
    const view = harness(request('preview'), source);
    await view.mount();
    await view.flush();
    expect(view.state()).toMatchObject({ phase: 'failed', view: null, error: 'network down' });
    view.unmount();
  });
});

const SESSION = resolve(__dirname, '../../../logs/phone-lab/2026-11-21_233928/saves');
describe.skipIf(!existsSync(SESSION))('read-back of the real phone-lab session', () => {
  const read = (kind: string): Uint8Array => new Uint8Array(readFileSync(resolve(SESSION, readdirSync(SESSION).find((entry) => entry.includes(`_${kind}_`))!)));
  const REAL_ID = 'scan-crc32:d2dbceda';
  const REAL_SHA = 'ba95124dc8ecca8b6671641e3760cfd3415955c92fa498b16007d2e6fbc7e219';

  it('shows its 119 preview cells and its 28 FREE / 127 OCCUPIED solver cells for its own scan, and nothing for another', async () => {
    const source = loaders(read('preview'), read('occupancy'));
    const preview = await loadSavedEsp32Voxels({ objectId: 'OBJ_0001', sessionId: 'staging', kind: 'preview', scanId: REAL_ID }, source);
    expect(preview.state === 'ready' && preview.view.kind === 'preview' && preview.view.artifact.voxelCount).toBe(119);
    const solver = await loadSavedEsp32Voxels({ objectId: 'OBJ_0001', sessionId: 'staging', kind: 'solver', scanId: REAL_ID, scanSha256: REAL_SHA }, source);
    expect(solver.state === 'ready' && solver.view.kind === 'solver' && [solver.view.occupancy.occupied.length / 3, solver.view.occupancy.free.length / 3]).toEqual([127, 28]);
    expect(await loadSavedEsp32Voxels({ objectId: 'OBJ_0001', sessionId: 'staging', kind: 'preview', scanId: 'scan-crc32:00000001' }, source)).toEqual({ state: 'stale' });
    expect(await loadSavedEsp32Voxels({ objectId: 'OBJ_0001', sessionId: 'staging', kind: 'solver', scanId: REAL_ID, scanSha256: 'c'.repeat(64) }, source)).toEqual({ state: 'stale' });
  });
});
