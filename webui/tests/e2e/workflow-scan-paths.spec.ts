import type { Page } from '@playwright/test';
import { test, expect, catalog } from './fixtures/device';
import type { DeviceApi } from './fixtures/api';
import { workflowActions } from './fixtures/visual-states';
import { driveJourneyXr, installJourneyXr, journeyXrState } from './fixtures/xr-session';
import { decodeScanArtifact } from '../../src/xr/scanArtifact';
import { createScanIdentity } from '../../src/xr/scanIdentity';
import { decodeOccupancySolverArtifact } from '../../src/xr/occupancySolverArtifact';
import type { WorkflowStep } from '../../src/domain/device';

/*
 * The operator paths around the 3D scan of the measurement workflow that the primary journey does not take: leaving a
 * scan half way and starting over, scanning again over a saved scan, and Wi-Fi dropping while the occupancy (the FREE /
 * OCCUPIED voxels the source calculation is constrained by) is uploaded. In every path the device must end up with
 * exactly one occupancy, bound to the scan it last accepted, before the workflow leaves the scanner.
 */

type Upload = { kind: string; bytes: Uint8Array; scanId: string | null };
type JourneyXr = { frame: (index: number) => void };

async function driveFrames(page: Page, first: number, count: number) {
  for (let index = first; index < first + count; index++) {
    await page.evaluate(i => (window as unknown as { __journeyXr: JourneyXr }).__journeyXr.frame(i), index);
    await page.waitForTimeout(50);
  }
}

function scanDevice(api: DeviceApi) {
  const workflow = api.status.workflow!;
  const enter = (step: WorkflowStep, actions = workflowActions[step]) => {
    Object.assign(workflow, { step, generation: workflow.generation + 1, allowed_actions: [...actions] });
  };
  const uploads: Upload[] = [];
  const commands: string[] = [];
  const device = { workflow, uploads, commands, wifiDownForOccupancy: false, occupancyAttempts: 0 };
  let lastScan: Uint8Array | null = null;
  let lastOccupancy: Upload | null = null;
  enter('SCAN_CHOICE');
  api.overrides.set('POST /api/control', async route => {
    const query = new URL(route.request().url()).searchParams;
    if (query.has('scan')) {
      const kind = query.get('kind') ?? 'scan';
      if (query.get('scan') === 'info') {
        const json = kind === 'scan' && lastScan !== null
          ? { ok: true, bytes: lastScan.length, id: createScanIdentity(lastScan).frameId }
          : kind === 'occupancy' && lastOccupancy !== null
            ? { ok: true, bytes: lastOccupancy.bytes.length, source_id: lastOccupancy.scanId }
            : { ok: false, error: 'ESP_ERR_NOT_FOUND' };
        await route.fulfill({ json });
        return;
      }
      if (query.get('scan') === 'load') {
        /* Reopening the scanner reads the scan the device holds back (the "continue / scan again" choice). */
        if (kind === 'scan' && lastScan !== null) {
          const nodeBuffer = (globalThis as unknown as { Buffer: { from(bytes: Uint8Array): never } }).Buffer;
          await route.fulfill({ status: 200, contentType: 'application/octet-stream', body: nodeBuffer.from(lastScan) });
        } else {
          await route.fulfill({ status: 404, json: { ok: false, error: 'ESP_ERR_NOT_FOUND' } });
        }
        return;
      }
      expect(query.get('scan')).toBe('save');
      if (kind === 'occupancy') {
        device.occupancyAttempts++;
        if (device.wifiDownForOccupancy) {
          await route.abort('internetdisconnected');
          return;
        }
      }
      const bytes = new Uint8Array(route.request().postDataBuffer()!);
      uploads.push({ kind, bytes, scanId: query.get('scan_id') });
      if (kind === 'occupancy') lastOccupancy = uploads[uploads.length - 1];
      if (kind === 'scan') {
        lastScan = bytes;
        lastOccupancy = null;
        Object.assign(workflow, { has_scan: true, scan_upload_status: 'verified' });
      }
      await route.fulfill({ json: { ok: true } });
      return;
    }
    const body = route.request().postDataJSON();
    expect(body.action).toBe('workflow_command');
    expect(body.generation).toBe(workflow.generation);
    if (body.command !== 'SCAN_COMPLETE') expect(workflow.allowed_actions).toContain(body.command);
    commands.push(body.command);
    switch (body.command) {
      case 'SCAN_START': enter('SCANNING'); break;
      case 'SCAN_COMPLETE':
        expect(workflow.step).toBe('SCANNING');
        enter('RECORD_READY'); break;
      case 'BACK': enter(workflow.step === 'SCANNING' || workflow.step === 'RECORD_READY' ? 'SCAN_CHOICE' : 'LAYOUT_READY'); break;
      default: throw new Error(`Unexpected command ${body.command}`);
    }
    await route.fulfill({ json: { ok: true, workflow } });
  });
  return device;
}

/* The occupancy last uploaded is bound to the scan last uploaded and carries both voxel classes. */
function expectOccupancyOfLastScan(uploads: Upload[]) {
  const scans = uploads.filter(upload => upload.kind === 'scan');
  const occupancies = uploads.filter(upload => upload.kind === 'occupancy');
  expect(scans.length).toBeGreaterThan(0);
  expect(occupancies.length).toBeGreaterThan(0);
  const scan = scans[scans.length - 1].bytes;
  expect(decodeScanArtifact(scan).pointCount).toBeGreaterThan(0);
  const frameId = createScanIdentity(scan).frameId;
  const last = occupancies[occupancies.length - 1];
  const decoded = decodeOccupancySolverArtifact(last.bytes);
  expect(decoded.header.frame_id, 'occupancy bound to the scan last saved').toBe(frameId);
  expect(last.scanId).toBe(frameId);
  expect(decoded.free.length / 3).toBeGreaterThan(0);
  expect(decoded.occupied.length / 3).toBeGreaterThan(0);
  expect(decoded.header.voxel_size_m, 'the ESP32 gets voxels no finer than 0.25 m').toBeGreaterThanOrEqual(0.25);
  return frameId;
}

for (const locale of ['en', 'uk'] as const) {
  test.describe(`scan paths ${locale}`, () => {
    test.use({ appLocale: locale });
    const t = catalog[locale];

    async function openScanChoice(page: Page) {
      await page.goto('/');
      await page.getByRole('navigation').getByRole('button', { name: t['workflow.nav'], exact: true }).click();
      await expect(page.locator('.workflow-page h2')).toHaveText(t['workflow.title.scan_choice']);
    }

    async function startScanner(page: Page, button: 'xr.start_scan' = 'xr.start_scan') {
      await page.locator('[data-command="SCAN_START"]').click();
      await expect(page.getByRole('heading', { name: t['scan.search_volume_title'], exact: true })).toBeVisible();
      const start = page.getByRole('button', { name: t[button], exact: true });
      await expect(start).toBeEnabled();
      await start.click();
    }

    test('leaving a scan half way uploads nothing; the next scan is the one the device keeps', async ({ page, api }) => {
      test.setTimeout(120_000);
      await installJourneyXr(page);
      const device = scanDevice(api);
      await openScanChoice(page);
      await startScanner(page);
      await expect.poll(() => journeyXrState(page)).toEqual({ requests: 1, active: true });
      await driveFrames(page, 0, 6);
      await page.locator('.console-floating-back').click();
      await expect(page.locator('.workflow-page h2')).toHaveText(t['workflow.title.scan_choice']);
      await expect.poll(async () => (await journeyXrState(page)).active, { message: 'the AR session ends with the scan' }).toBe(false);
      expect(device.commands).toEqual(['SCAN_START', 'BACK']);
      expect(device.uploads, 'a cancelled scan saves no scan and no voxels').toEqual([]);

      await startScanner(page);
      await expect.poll(() => journeyXrState(page)).toEqual({ requests: 2, active: true });
      await driveJourneyXr(page);
      await page.locator('.xr-finish-button').click();
      await expect(page.locator('.workflow-page h2')).toHaveText(t['workflow.title.record_ready']);
      expect(device.commands).toEqual(['SCAN_START', 'BACK', 'SCAN_START', 'SCAN_COMPLETE']);
      expect(device.uploads.filter(upload => upload.kind === 'scan')).toHaveLength(1);
      expect(device.uploads.filter(upload => upload.kind === 'occupancy')).toHaveLength(1);
      expectOccupancyOfLastScan(device.uploads);
    });

    test('scanning again over a saved scan replaces it and its voxels', async ({ page, api }) => {
      test.setTimeout(150_000);
      await installJourneyXr(page);
      const device = scanDevice(api);
      await openScanChoice(page);
      await startScanner(page);
      await expect.poll(() => journeyXrState(page)).toEqual({ requests: 1, active: true });
      await driveJourneyXr(page);
      await page.locator('.xr-finish-button').click();
      await expect(page.locator('.workflow-page h2')).toHaveText(t['workflow.title.record_ready']);
      const first = expectOccupancyOfLastScan(device.uploads);

      await page.locator('[data-command="BACK"]').click();
      await expect(page.locator('.workflow-page h2')).toHaveText(t['workflow.title.scan_choice']);
      /* The workflow scanner writes the staging scan: scanning again is a fresh scan over it (no "continue"). */
      await startScanner(page);
      await expect.poll(() => journeyXrState(page)).toEqual({ requests: 2, active: true });
      await driveFrames(page, 8, 24); /* another path through the room: another scan */
      await page.locator('.xr-finish-button').click();
      await expect(page.locator('.workflow-page h2')).toHaveText(t['workflow.title.record_ready']);
      const second = expectOccupancyOfLastScan(device.uploads);
      expect(second, 'the fresh scan is a new scan').not.toBe(first);
      expect(device.uploads.filter(upload => upload.kind === 'scan')).toHaveLength(2);
      expect(device.commands).toEqual(['SCAN_START', 'SCAN_COMPLETE', 'BACK', 'SCAN_START', 'SCAN_COMPLETE']);
    });

    test('Wi-Fi dropping during the voxel upload keeps the scanner open until the retry saves them', async ({ page, api }) => {
      test.setTimeout(120_000);
      await installJourneyXr(page);
      const device = scanDevice(api);
      device.wifiDownForOccupancy = true;
      /* The browser's own report of the simulated drop. */
      api.expectedConsoleErrors.push('Failed to load resource: net::ERR_INTERNET_DISCONNECTED');
      await openScanChoice(page);
      await startScanner(page);
      await expect.poll(() => journeyXrState(page)).toEqual({ requests: 1, active: true });
      await driveJourneyXr(page);
      await page.locator('.xr-finish-button').click();

      const retry = page.getByRole('button', { name: t['scan.retry.occupancy'], exact: true });
      await expect(retry, 'the failed voxel upload offers a retry').toBeVisible();
      await expect(page.getByText(t['scan.volume.occupancy_not_ready'], { exact: true })).toBeVisible();
      expect(device.occupancyAttempts).toBeGreaterThan(0);
      expect(device.uploads.filter(upload => upload.kind === 'scan')).toHaveLength(1);
      expect(device.uploads.filter(upload => upload.kind === 'occupancy')).toHaveLength(0);
      expect(device.commands, 'the workflow must not leave the scanner without the voxels').toEqual(['SCAN_START']);
      expect(device.workflow.step).toBe('SCANNING');

      device.wifiDownForOccupancy = false;
      await retry.click();
      await expect(page.locator('.workflow-page h2')).toHaveText(t['workflow.title.record_ready']);
      expect(device.commands).toEqual(['SCAN_START', 'SCAN_COMPLETE']);
      expect(device.uploads.filter(upload => upload.kind === 'scan'), 'the retry does not upload the scan again').toHaveLength(1);
      expect(device.uploads.filter(upload => upload.kind === 'occupancy')).toHaveLength(1);
      expectOccupancyOfLastScan(device.uploads);
    });
  });
}
