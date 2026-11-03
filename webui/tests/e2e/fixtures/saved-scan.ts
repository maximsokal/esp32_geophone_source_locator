import type { Page } from '@playwright/test';
import { expect, catalog } from './device';
import type { DeviceApi } from './api';
import { encodeScanArtifact } from '../../../src/xr/scanArtifact';
import { createScanIdentity } from '../../../src/xr/scanIdentity';
import { defaultRegistrationTagSizes, encodeRegistrationArtifact, type RegistrationArtifact } from '../../../src/xr/fiducial/registrationArtifact';
import { makeValidScanSnapshot } from '../../xr/support/validScanSnapshot';
import { openSection } from '../helpers/layout';

const GEOMETRY_MS = 500;
const PODS = [
  { pod: 0, node_id: 0, computed: true, assigned: true, leveled: true, computed_at_ms: GEOMETRY_MS },
  { pod: 1, node_id: 1, computed: true, assigned: true, leveled: true, x_mm: 3000, y_mm: 0, z_mm: 0, computed_at_ms: GEOMETRY_MS },
  { pod: 2, node_id: 2, computed: true, assigned: true, leveled: true, x_mm: 0, y_mm: 4000, z_mm: 0, computed_at_ms: GEOMETRY_MS },
];
const tag = (node: 'MAIN' | 'G1' | 'G2', tagId: number, position: [number, number, number]) =>
  ({ tag_id: tagId, node, observation_count: 9, inlier_count: 8, position_scan_m: position, spread_m: 0.01 });

/** The scan the device already holds, a tag registration bound to it, and the geometry the registration was made against. */
export function installSavedScan(api: DeviceApi, withTags: boolean) {
  const scan = encodeScanArtifact(makeValidScanSnapshot());
  const frameId = createScanIdentity(scan).frameId;
  const registration: RegistrationArtifact = {
    schema: 1, frame: 'webxr_local_floor', scan_frame_id: frameId, source: 'apriltag', family: 'tag36h11',
    geometry_computed_at_ms: GEOMETRY_MS, created_at_ms: 600, tag_size_m: defaultRegistrationTagSizes(),
    bindings: withTags ? [tag('MAIN', 0, [0, 0, 0]), tag('G1', 1, [2.8, 0, 0]), tag('G2', 2, [0, 4.1, 0])] : [],
    sensor_to_scan: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1], mirrored: false,
    residual_rms_m: 0.01, threshold_m: 0.05, per_node: [], tier: 'minimum', valid: true, reason: null,
  };
  const registrationText = encodeRegistrationArtifact(registration);
  const workflow = api.status.workflow!;
  api.overrides.set('GET /api/localization/runs', async route => {
    await route.fulfill({ json: { geometry: { available: true, pods: PODS }, cs_pairs: { available: false, pairs: [] }, picks: [], runs: [] } });
  });
  api.overrides.set('POST /api/control', async route => {
    const query = new URL(route.request().url()).searchParams;
    if (!query.has('scan')) { await route.fallback(); return; }
    const action = query.get('scan');
    const kind = query.get('kind') ?? 'scan';
    if (action === 'info') {
      await route.fulfill({ json: kind === 'scan' ? { ok: true, bytes: scan.length, id: frameId }
        : kind === 'registration' ? { ok: true, bytes: registrationText.length, id: null }
          : { ok: true, bytes: 16, source_id: frameId } });
      return;
    }
    if (action === 'load') {
      const nodeBuffer = (globalThis as unknown as { Buffer: { from(bytes: Uint8Array | string): never } }).Buffer;
      const body = kind === 'scan' ? nodeBuffer.from(scan) : kind === 'registration' ? nodeBuffer.from(registrationText) : null;
      if (body === null) await route.fulfill({ status: 404, json: { ok: false, error: 'ESP_ERR_NOT_FOUND' } });
      else await route.fulfill({ status: 200, contentType: 'application/octet-stream', body });
      return;
    }
    await route.fulfill({ json: { ok: true, action: kind } });
  });
}

/** Objects -> the recorded session -> the 3D scan of that session, the screen the operator reads the scan information on. */
export async function openSavedSessionScan(page: Page, locale: 'en' | 'uk') {
  await openSection(page, 'objects', locale);
  await page.getByRole('button', { name: /S000001/ }).first().click();
  await page.getByRole('button', { name: catalog[locale]['objects.scan_add'], exact: true }).click();
}

