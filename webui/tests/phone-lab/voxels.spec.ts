import { expect, test, type APIRequestContext, type Page } from '@playwright/test';
import { syntheticRoomScan } from '../support/syntheticRoomScan';
import { encodeScanArtifact } from '../../src/xr/scanArtifact';
import { createScanIdentity } from '../../src/xr/scanIdentity';
import { buildSessionOccupancyArtifact } from '../../src/xr/occupancySolverArtifact';
import { buildVoxelPreviewArtifact } from '../../src/xr/voxelPreview';

/*
 * The real app on the phone-lab fake device, in Chromium (SwiftShader WebGL). Seeds a synthetic, public room scan with its ESP32 derivatives through the
 * lab API exactly as the phone does (scan_id on save), then checks what the CANVAS draws, pixel by pixel, for Surface Mesh / TFT voxels / Solver voxels.
 */
const Q = 'object_id=OBJ_0001&session_id=staging';
const octet = { 'content-type': 'application/octet-stream' };

interface Seed { scan: Uint8Array; occupancy: Uint8Array; preview: Uint8Array; frameId: string }

function makeSeed(offset: readonly [number, number, number] = [0, 0, 0]): Seed {
  const snapshot = syntheticRoomScan({ offset });
  const scan = encodeScanArtifact(snapshot);
  const identity = createScanIdentity(scan);
  const occupancy = buildSessionOccupancyArtifact(snapshot, identity, 1);
  const preview = buildVoxelPreviewArtifact(snapshot, identity, { occupancyArtifact: occupancy, identity });
  return { scan, occupancy, preview, frameId: identity.frameId };
}

async function save(request: APIRequestContext, kind: 'scan' | 'occupancy' | 'preview', bytes: Uint8Array, frameId?: string): Promise<number> {
  const query = kind === 'scan' ? '' : `&kind=${kind}&scan_id=${encodeURIComponent(frameId!)}`;
  return (await request.post(`/api/control?scan=save&${Q}${query}`, { data: Buffer.from(bytes), headers: octet })).status();
}
async function clear(request: APIRequestContext): Promise<void> {
  for (const kind of ['scan', 'occupancy', 'preview', 'tags', 'registration']) await request.post(`/api/control?scan=delete&${Q}&kind=${kind}`);
}
async function load(request: APIRequestContext, kind: 'scan' | 'occupancy' | 'preview'): Promise<{ status: number; bytes: Buffer }> {
  const response = await request.post(`/api/control?scan=load&${Q}${kind === 'scan' ? '' : `&kind=${kind}`}`);
  return { status: response.status(), bytes: Buffer.from(await response.body()) };
}

interface CanvasStats { width: number; height: number; lit: number; luma: number; teal: number; white: number; grid: number[]; colour: number[] }
/** Screenshot of the scan canvas, decoded in the page: the share of pixels that differ from the background and a coarse luminance grid. */
async function canvasStats(page: Page): Promise<CanvasStats> {
  const canvas = page.locator('.scan-webgl-canvas canvas').first();
  await expect(canvas).toBeVisible();
  const shot = (await canvas.screenshot()).toString('base64');
  return page.evaluate(async (base64) => {
    const blob = await (await fetch(`data:image/png;base64,${base64}`)).blob();
    const bitmap = await createImageBitmap(blob);
    const surface = document.createElement('canvas');
    surface.width = bitmap.width; surface.height = bitmap.height;
    const context = surface.getContext('2d')!;
    context.drawImage(bitmap, 0, 0);
    const data = context.getImageData(0, 0, bitmap.width, bitmap.height).data;
    const at = (x: number, y: number): [number, number, number] => { const o = (y * bitmap.width + x) * 4; return [data[o], data[o + 1], data[o + 2]]; };
    const background = at(2, 2);
    let lit = 0; let lumaSum = 0; let teal = 0; let white = 0;
    const cells = 12; const grid: number[] = new Array(cells * cells).fill(0);
    const colour: number[] = new Array(cells * cells * 3).fill(0); const seen: number[] = new Array(cells * cells).fill(0);
    for (let y = 0; y < bitmap.height; y += 1) for (let x = 0; x < bitmap.width; x += 1) {
      const p = at(x, y);
      lumaSum += (p[0] + p[1] + p[2]) / 765;
      if (p[1] > 100 && p[2] > 100 && p[1] >= p[0] + 40 && p[2] >= p[0] + 40) teal += 1;
      if (p[0] > 200 && p[1] > 200 && p[2] > 200) white += 1;
      const cell = Math.min(cells - 1, Math.floor(y / bitmap.height * cells)) * cells + Math.min(cells - 1, Math.floor(x / bitmap.width * cells));
      colour[cell * 3] += p[0]; colour[cell * 3 + 1] += p[1]; colour[cell * 3 + 2] += p[2]; seen[cell] += 1;
      const different = Math.abs(p[0] - background[0]) + Math.abs(p[1] - background[1]) + Math.abs(p[2] - background[2]) > 40;
      if (different) { lit += 1; grid[Math.min(cells - 1, Math.floor(y / bitmap.height * cells)) * cells + Math.min(cells - 1, Math.floor(x / bitmap.width * cells))] += 1; }
    }
    for (let c = 0; c < cells * cells; c += 1) for (let k = 0; k < 3; k += 1) colour[c * 3 + k] = seen[c] === 0 ? 0 : colour[c * 3 + k] / seen[c] / 255;
    return { width: bitmap.width, height: bitmap.height, lit: lit / (bitmap.width * bitmap.height), luma: lumaSum / (bitmap.width * bitmap.height), teal: teal / (bitmap.width * bitmap.height), white: white / (bitmap.width * bitmap.height), grid, colour };
  }, shot);
}
/** Mean absolute difference of the mean colour of the 12 x 12 cells (0 = same picture, about 0.3 = a clearly different one). */
function difference(a: CanvasStats, b: CanvasStats): number {
  let diff = 0;
  for (let i = 0; i < a.colour.length; i += 1) diff += Math.abs(a.colour[i] - b.colour[i]);
  return diff / a.colour.length;
}
async function choose(page: Page, value: 'mesh' | 'esp32-preview' | 'esp32-solver'): Promise<void> {
  await page.locator('select').filter({ has: page.locator('option[value="esp32-preview"]') }).first().selectOption(value);
}
async function settle(page: Page, ms = 1500): Promise<void> { await page.waitForTimeout(ms); }

test.describe('ESP32 voxels in the real app on the phone lab', () => {
  test.beforeEach(async ({ request }) => { await clear(request); });

  test('VOX-02: what the phone saved is what the device returns, byte for byte, and the artifacts are the scan\'s own', async ({ request }) => {
    const seed = makeSeed();
    expect(await save(request, 'scan', seed.scan)).toBe(200);
    expect(await save(request, 'occupancy', seed.occupancy, seed.frameId)).toBe(200);
    expect(await save(request, 'preview', seed.preview, seed.frameId)).toBe(200);
    for (const [kind, bytes] of [['scan', seed.scan], ['occupancy', seed.occupancy], ['preview', seed.preview]] as const) {
      const loaded = await load(request, kind);
      expect(loaded.status, kind).toBe(200);
      expect(loaded.bytes.equals(Buffer.from(bytes)), `${kind} returned byte for byte`).toBe(true);
    }
  });

  test('VOX-09/10/11: Surface Mesh -> TFT voxels -> Solver voxels -> Surface Mesh really changes what the canvas draws, and the mesh comes back unchanged', async ({ page, request }) => {
    const seed = makeSeed();
    await save(request, 'scan', seed.scan); await save(request, 'occupancy', seed.occupancy, seed.frameId); await save(request, 'preview', seed.preview, seed.frameId);
    await page.goto('/');
    await expect(page.locator('select option[value="esp32-preview"]').first()).toBeAttached();
    await settle(page, 3000);
    const mesh = await canvasStats(page);
    expect(mesh.lit, 'the surface mesh draws something').toBeGreaterThan(0.01);

    await choose(page, 'esp32-preview');
    await expect(page.getByText('The ESP32 holds no voxels for this scan.')).toHaveCount(0);
    await settle(page);
    const preview = await canvasStats(page);
    expect(preview.lit, 'the TFT voxels draw something').toBeGreaterThan(0.001);
    expect(preview.teal, 'the voxel slab is on the canvas').toBeGreaterThan(0.1);
    expect(mesh.teal, 'the surface mesh has none of that colour').toBeLessThan(0.08);
    await expect(page.locator('.scan-voxel-overlay, .scan-mode-overlay, [data-voxel-kind]').filter({ hasText: /TFT VOXELS/i }).first()).toBeVisible();
    expect(difference(mesh, preview), 'the TFT voxels are not the mesh').toBeGreaterThan(0.03);

    await choose(page, 'esp32-solver');
    await settle(page);
    const solver = await canvasStats(page);
    expect(solver.lit).toBeGreaterThan(0.001);
    expect(solver.teal, 'the solver voxels are on the canvas').toBeGreaterThan(0.1);
    await expect(page.locator('[data-voxel-kind="solver"]').first()).toBeVisible();
    expect(difference(mesh, solver)).toBeGreaterThan(0.02);

    await choose(page, 'mesh');
    await settle(page, 2500);
    const back = await canvasStats(page);
    expect(difference(mesh, back), 'the mesh is restored').toBeLessThan(0.01);
    expect(Math.abs(back.lit - mesh.lit)).toBeLessThan(0.02);
    expect(back.teal).toBeLessThan(0.08);
  });

  test('VOX-03: closing the viewer (a page reload) and opening it again restores the TFT and solver voxels from the device', async ({ page, request }) => {
    const seed = makeSeed();
    await save(request, 'scan', seed.scan); await save(request, 'occupancy', seed.occupancy, seed.frameId); await save(request, 'preview', seed.preview, seed.frameId);
    for (let visit = 0; visit < 2; visit += 1) {
      await page.goto('/');
      await page.reload();
      await expect(page.locator('select option[value="esp32-preview"]').first()).toBeAttached();
      await settle(page, 2500);
      for (const [value, kind] of [['esp32-preview', 'preview'], ['esp32-solver', 'solver']] as const) {
        await choose(page, value);
        await settle(page);
        await expect(page.getByText('The ESP32 holds no voxels for this scan.'), `${kind} on visit ${visit + 1}`).toHaveCount(0);
        expect((await canvasStats(page)).teal, `${kind} voxels are drawn after reopening`).toBeGreaterThan(0.1);
      }
    }
  });

  test('VOX-08: a missing preview is said to be missing and the canvas does NOT keep drawing the mesh in its place', async ({ page, request }) => {
    const seed = makeSeed();
    await save(request, 'scan', seed.scan); await save(request, 'occupancy', seed.occupancy, seed.frameId); await save(request, 'preview', seed.preview, seed.frameId);
    await request.post(`/api/control?scan=delete&${Q}&kind=preview`);
    await page.goto('/');
    await expect(page.locator('select option[value="esp32-preview"]').first()).toBeAttached();
    await settle(page, 3000);
    const mesh = await canvasStats(page);
    await choose(page, 'esp32-preview');
    await expect(page.getByText('The ESP32 holds no voxels for this scan.')).toBeVisible();
    await settle(page);
    const missing = await canvasStats(page);
    expect(missing.luma, 'the canvas is nearly empty (the bounding box only), not the mesh').toBeLessThan(mesh.luma * 0.8);
    expect(difference(mesh, missing), 'a different picture than the mesh').toBeGreaterThan(0.03);
    expect(missing.teal, 'and no voxels either').toBeLessThan(0.08);
  });

  test('VOX-04/16: another scan\'s derivatives are never shown: after the scan is replaced, the old occupancy is refused and not drawn as the new scan\'s', async ({ request }) => {
    const first = makeSeed();
    await save(request, 'scan', first.scan);
    expect(await save(request, 'occupancy', first.occupancy, first.frameId)).toBe(200);
    const second = makeSeed([1, 1, 0]);
    expect(second.frameId).not.toBe(first.frameId);
    await save(request, 'scan', second.scan);
    expect(await save(request, 'occupancy', first.occupancy, first.frameId), 'a derivative of the replaced scan').toBe(409);
    expect((await request.post(`/api/control?scan=load&${Q}&kind=occupancy&scan_id=${encodeURIComponent(first.frameId)}`)).status()).toBe(409);
  });
});
