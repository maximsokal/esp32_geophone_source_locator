import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, expect, it } from 'vitest';

import { decodeScanArtifactWithIdentity } from '../../src/xr/scanArtifact';
import { decodeOccupancySolverArtifact, occupancyFrameIdForScan } from '../../src/xr/occupancySolverArtifact';
import { buildVoxelPreviewArtifact, decodeVoxelPreviewArtifact } from '../../src/xr/voxelPreview';

/*
 * Read-only replay of a real phone-lab scan (issues #43 and #44): the bytes the phone sent to the ESP32 on 2026-11-21 23:39 are decoded with the
 * production decoders, so every later claim about "what the ESP32 received" starts from measured numbers instead of the phone's display counters.
 *
 * The session lives under logs/ (git-ignored; phone-lab traces are not committed). Without it the suite is skipped, never faked.
 */
const SESSION = resolve(__dirname, '../../../logs/phone-lab/2026-11-21_233928/saves');
const available = existsSync(SESSION);

function saved(kind: string): Uint8Array {
  const name = readdirSync(SESSION).find((entry) => entry.includes(`_${kind}_`));
  if (name === undefined) throw new Error(`no ${kind} in ${SESSION}`);
  return new Uint8Array(readFileSync(resolve(SESSION, name)));
}

describe.skipIf(!available)('phone-lab replay 2026-11-21_233928 (OBJ_0001/staging)', () => {
  it('the saved byte counts are the ones the issues record', () => {
    expect(saved('scan').byteLength).toBe(126195);
    expect(saved('occupancy').byteLength).toBe(1368);
    expect(saved('preview').byteLength).toBe(415);
    expect(saved('tags').byteLength).toBe(862);
  });

  it('decodes the TFT preview GXPRV1 exactly: 119 cells of 0.25 m, legacy v1 (unregistered), CRC and size valid, bound to the scan', () => {
    const scan = saved('scan');
    const preview = decodeVoxelPreviewArtifact(saved('preview'), scan);
    expect(preview.voxelCount).toBe(119);
    expect(preview.indices).toHaveLength(119);
    expect(preview.faceMasks).toHaveLength(119);
    expect(preview.voxelSizeMeters).toBe(0.25);
    expect(preview.registration).toBeNull();
    expect(preview.indexWidth).toBe(2);
    const volume = preview.dimensions[0] * preview.dimensions[1] * preview.dimensions[2];
    expect(volume).toBeGreaterThan(0);
    expect(volume).toBeLessThanOrEqual(0x10000);
    for (let i = 0; i < preview.indices.length; i += 1) {
      expect(preview.indices[i]).toBeLessThan(volume);
      if (i > 0) expect(preview.indices[i], 'indices are strictly increasing (sorted, no duplicates)').toBeGreaterThan(preview.indices[i - 1]);
      expect(preview.faceMasks[i], 'a surface cell has at least one exposed face').not.toBe(0);
      expect(preview.faceMasks[i]).toBeLessThan(64);
    }
  });

  it('decodes the solver occupancy GOCCUPY1 exactly: 28 FREE and 127 OCCUPIED cells, 0.25 m, bound to the scan by CRC and SHA-256', () => {
    const scan = saved('scan');
    const occupancy = decodeOccupancySolverArtifact(saved('occupancy'));
    const { identity } = decodeScanArtifactWithIdentity(scan);
    expect(occupancy.header.schema).toBe(2);
    expect(occupancy.header.free_count).toBe(28);
    expect(occupancy.header.occupied_count).toBe(127);
    expect(occupancy.free).toHaveLength(28 * 3);
    expect(occupancy.occupied).toHaveLength(127 * 3);
    expect(occupancy.header.voxel_size_m).toBe(0.25);
    expect(occupancy.header.saturated).toBe(false);
    expect(occupancy.header.registration).toBeNull();
    expect(occupancy.header.frame_id).toBe(occupancyFrameIdForScan(scan, identity));
    expect(occupancy.header.scan_sha256).toBe(identity.sha256);
  });

  it('the phone display counters (294 FREE / 397 UNKNOWN) are NOT what was serialised', () => {
    const occupancy = decodeOccupancySolverArtifact(saved('occupancy'));
    expect(occupancy.header.free_count).not.toBe(294);
    const preview = decodeVoxelPreviewArtifact(saved('preview'), saved('scan'));
    expect(preview.voxelCount).not.toBe(occupancy.header.occupied_count);   /* 119 surface cells of the TFT preview vs 127 solver OCCUPIED cells */
  });

  it('the scan restores its TSDF input: 1063 certified points, the figure SCAN_TSDF_READY logged', () => {
    const { snapshot } = decodeScanArtifactWithIdentity(saved('scan'));
    expect(snapshot.tsdfPointCount).toBe(1063);
    expect(snapshot.tsdfPositions).toHaveLength(1063 * 3);
    expect(snapshot.tsdfNormals).toHaveLength(1063 * 3);
    expect(snapshot.tsdfCoverageConfidence).toHaveLength(1063);
  });

  it('rebuilding the preview from the saved scan and occupancy reproduces the saved 415 bytes (the encoder is deterministic)', () => {
    const scan = saved('scan');
    const { snapshot, identity } = decodeScanArtifactWithIdentity(scan);
    const rebuilt = buildVoxelPreviewArtifact(snapshot, identity, { occupancyArtifact: saved('occupancy'), identity, registration: null });
    expect(Array.from(rebuilt)).toEqual(Array.from(saved('preview')));
  });
});
