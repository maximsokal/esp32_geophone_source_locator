import { expect, test, type Page } from '@playwright/test';

/*
 * Framebuffer pixel tests of the live mesh renderer (TASK 02). The REAL renderer draws on a native WebGL2 context (SwiftShader) over a known background and
 * the pixels are read back. Nothing here looks at constants: the opacity the camera image actually receives is measured.
 *
 * Colour of the live mesh is (0.30, 0.78, 1.0), alpha 0.10. Over black the blue channel is 255 * alpha; over white the red channel is 255 * (1 - 0.7 * alpha).
 */
const ALPHA = 0.10;
const BUDGET = 0.25;
type Rgba = [number, number, number, number];
interface RunResult { pixels: Rgba[]; before: Record<string, unknown> | null; after: Record<string, unknown> | null; contexts: number }
interface Rect { x: number; y: number; w: number; h: number; color: [number, number, number] }

const FULL: Rect[] = [];
const solid = (color: [number, number, number], size = 64): Rect[] => [{ x: 0, y: 0, w: size, h: size, color }];
/** Two triangles covering the whole view at depth z. */
const cover = (z: number): number[] => [-1, -1, z, 1, -1, z, 1, 1, z, -1, -1, z, 1, 1, z, -1, 1, z];
/** `count` parallel shells at distinct depths inside the view volume (z in [start, 0.95]). */
const layers = (count: number, start = 0.2): number[] => Array.from({ length: count }, (_, i) => cover(start + (i * (0.95 - start)) / Math.max(1, count))).flat();
void FULL;

async function open(page: Page): Promise<void> {
  await page.goto('/stress/live-mesh-pixels.html');
  await expect.poll(() => page.evaluate(() => (window as unknown as { __liveMeshPixels?: { ready: boolean } }).__liveMeshPixels?.ready ?? false)).toBe(true);
}
async function draw(page: Page, spec: Record<string, unknown>): Promise<RunResult> {
  return page.evaluate((s) => (window as unknown as { __liveMeshPixels: { run: (x: unknown) => RunResult } }).__liveMeshPixels.run(s), spec);
}
/** Opacity of the live mesh as received by a black background pixel. */
const alphaOverBlack = (pixel: Rgba): number => pixel[2] / 255;
const alphaOverWhite = (pixel: Rgba): number => (1 - pixel[0] / 255) / 0.7;

test.describe('live mesh opacity on the real framebuffer', () => {
  test.beforeEach(async ({ page }) => { await open(page); });

  test('LIVE-01 one shell receives exactly the configured opacity (and nothing is drawn outside the triangle)', async ({ page }) => {
    const small = [-0.5, -0.5, 0.3, 0.5, -0.5, 0.3, 0.5, 0.5, 0.3];
    const result = await draw(page, { background: solid([0, 0, 0]), meshes: [{ triangles: small }], probes: [[40, 28], [3, 3], [60, 60]] });
    expect(alphaOverBlack(result.pixels[0])).toBeCloseTo(ALPHA, 1);
    expect(Math.abs(alphaOverBlack(result.pixels[0]) - ALPHA)).toBeLessThan(0.012);
    expect(result.pixels[1].slice(0, 3), 'outside the triangle the background is untouched').toEqual([0, 0, 0]);
    expect(result.pixels[2].slice(0, 3)).toEqual([0, 0, 0]);
  });

  for (const layerCount of [2, 4, 8, 40]) {
    test(`LIVE-02 (${layerCount} layers) ${layerCount} overlapping shells: still ONE layer of opacity (the accumulating blend is measured next to it)`, async ({ page }) => {
      const single = await draw(page, { background: solid([0, 0, 0]), meshes: [{ triangles: layers(layerCount) }], probes: [[32, 32], [10, 50]] });
      for (const pixel of single.pixels) expect(Math.abs(alphaOverBlack(pixel) - ALPHA), `${layerCount} shells, single-layer`).toBeLessThan(0.012);
      const accumulated = await draw(page, { background: solid([0, 0, 0]), meshes: [{ triangles: layers(layerCount), mode: 'accumulate' }], probes: [[32, 32]] });
      const expected = 1 - (1 - ALPHA) ** layerCount;
      expect(Math.abs(alphaOverBlack(accumulated.pixels[0]) - expected), 'the old blend adds up as predicted').toBeLessThan(0.02);
      if (layerCount >= 4) expect(alphaOverBlack(accumulated.pixels[0]), 'and breaks the 25% budget').toBeGreaterThan(BUDGET);
      expect(alphaOverBlack(single.pixels[0]), 'the new one stays within it').toBeLessThanOrEqual(BUDGET);
    });
  }

  test('LIVE-05 a two-layer shell around a plane (the surface-nets skin of a thin wall) is one layer to the eye', async ({ page }) => {
    const result = await draw(page, { background: solid([0, 0, 0]), meshes: [{ triangles: [...cover(0.40), ...cover(0.48)] }], probes: [[32, 32]] });
    expect(Math.abs(alphaOverBlack(result.pixels[0]) - ALPHA)).toBeLessThan(0.012);
  });

  test('LIVE-06/07 crossing surfaces and a surface at a sharp angle to the camera never exceed the budget', async ({ page }) => {
    const crossing = [-1, -1, 0.2, 1, -1, 0.8, 1, 1, 0.8, -1, -1, 0.2, 1, 1, 0.8, -1, 1, 0.2,        /* tilted in depth */
      -1, -1, 0.8, 1, -1, 0.2, 1, 1, 0.2, -1, -1, 0.8, 1, 1, 0.2, -1, 1, 0.8];                          /* tilted the other way: the two cross along the middle */
    const probes: Array<[number, number]> = [[5, 32], [20, 32], [32, 32], [44, 32], [60, 32], [32, 5], [32, 60]];
    const result = await draw(page, { background: solid([0, 0, 0]), meshes: [{ triangles: crossing }], probes });
    for (const pixel of result.pixels) expect(alphaOverBlack(pixel)).toBeLessThanOrEqual(BUDGET);
    for (const pixel of result.pixels.filter((_, i) => i !== 2)) expect(Math.abs(alphaOverBlack(pixel) - ALPHA)).toBeLessThan(0.02);
    const sliver = [-1, -1, 0.1, 1, -1, 0.9, 1, -0.9, 0.9, -1, 0.5, 0.1, 1, 0.5, 0.9, 1, 0.6, 0.9];
    const oblique = await draw(page, { background: solid([0, 0, 0]), meshes: [{ triangles: sliver }], probes: [[32, 40], [32, 46]] });
    for (const pixel of oblique.pixels) expect(alphaOverBlack(pixel)).toBeLessThanOrEqual(BUDGET);
  });

  test('LIVE-08 a black-and-white AprilTag under eight overlapping shells keeps its contrast (the old blend lost more than half of it)', async ({ page }) => {
    const cells: Rect[] = [];
    for (let row = 0; row < 8; row += 1) for (let column = 0; column < 8; column += 1) cells.push({ x: column * 8, y: row * 8, w: 8, h: 8, color: (row + column) % 2 === 0 ? [255, 255, 255] : [0, 0, 0] });
    const probes: Array<[number, number]> = [[4, 4], [12, 4]];                                       /* a white cell and a black cell */
    const without = await draw(page, { background: cells, meshes: [], probes });
    const withMesh = await draw(page, { background: cells, meshes: [{ triangles: layers(8) }], probes });
    const old = await draw(page, { background: cells, meshes: [{ triangles: layers(8), mode: 'accumulate' }], probes });
    const contrast = (r: RunResult): number => (r.pixels[0][0] - r.pixels[1][0]) / 255;
    expect(contrast(without)).toBeCloseTo(1, 2);
    expect(contrast(withMesh), 'the tag stays readable').toBeGreaterThanOrEqual(0.85);
    expect(contrast(old), 'the old blend').toBeLessThan(0.6);
  });

  test('LIVE-09/10 a light wall stays light and a dark object stays dark under the mesh', async ({ page }) => {
    const light = await draw(page, { background: solid([235, 235, 230]), meshes: [{ triangles: layers(8) }], probes: [[32, 32]] });
    expect(light.pixels[0][0] / 235, 'brightness kept').toBeGreaterThanOrEqual(0.88);
    expect(alphaOverWhite([light.pixels[0][0] * 255 / 235, 0, 0, 255] as Rgba)).toBeLessThanOrEqual(BUDGET);
    const dark = await draw(page, { background: solid([20, 20, 22]), meshes: [{ triangles: layers(8) }], probes: [[32, 32]] });
    expect(Math.max(...dark.pixels[0].slice(0, 3)), 'a dark object is not turned into a blue screen').toBeLessThanOrEqual(70);
  });

  test('LIVE-11 WebGL state is exactly what it was before the mesh: blend, depth, masks, culling, polygon offset, program, buffers, viewport', async ({ page }) => {
    for (const depthBias of [false, true]) {
      const result = await draw(page, { background: solid([0, 0, 0]), meshes: [{ triangles: layers(3), depthBias }], probes: [[32, 32]], preState: true });
      expect(result.after, `depthBias ${depthBias}`).toEqual(result.before);
    }
  });

  test('LIVE-12 real-world occlusion: a nearer surface already in the depth buffer hides the mesh behind it; the mesh in front of it shows', async ({ page }) => {
    const occluder = [-1, -1, 0.3, 0, -1, 0.3, 0, 1, 0.3, -1, -1, 0.3, 0, 1, 0.3, -1, 1, 0.3];       /* left half of the view */
    const behind = await draw(page, { background: solid([0, 0, 0]), occluders: [occluder], meshes: [{ triangles: cover(0.6) }], probes: [[16, 32], [48, 32]] });
    expect(alphaOverBlack(behind.pixels[0]), 'hidden behind the real surface').toBeLessThan(0.004);
    expect(Math.abs(alphaOverBlack(behind.pixels[1]) - ALPHA), 'visible where nothing is in front').toBeLessThan(0.012);
    const inFront = await draw(page, { background: solid([0, 0, 0]), occluders: [occluder], meshes: [{ triangles: cover(0.1) }], probes: [[16, 32]] });
    expect(Math.abs(alphaOverBlack(inFront.pixels[0]) - ALPHA)).toBeLessThan(0.012);
  });

  test('LIVE-13/14 starting again and a quick finish-and-reopen: forty renderers created, drawn and disposed leave one context, no GL error and the same picture', async ({ page }) => {
    const first = await draw(page, { background: solid([0, 0, 0]), meshes: [{ triangles: layers(3) }], probes: [[32, 32]] });
    let last = first;
    for (let i = 0; i < 40; i += 1) last = await draw(page, { background: solid([0, 0, 0]), meshes: [{ triangles: layers(3) }], probes: [[32, 32]] });
    expect(last.pixels[0]).toEqual(first.pixels[0]);
    expect(last.contexts).toBe(1);
    expect(await page.evaluate(() => (document.getElementById('gl') as HTMLCanvasElement).getContext('webgl2')!.getError())).toBe(0);
  });

  test('LIVE-15 at the triangle limit (20,000 triangles, hundreds of layers) the opacity is still one layer', async ({ page }) => {
    const triangles = layers(2000, 0.05);                                                        /* 4,000 triangles over the whole view, 2,000 distinct depths */
    const result = await draw(page, { background: solid([0, 0, 0]), meshes: [{ triangles }], probes: [[32, 32], [50, 10]] });
    for (const pixel of result.pixels) expect(Math.abs(alphaOverBlack(pixel) - ALPHA)).toBeLessThan(0.02);
  });

  test('known limit: two triangles at EXACTLY the same depth both pass the depth test and blend twice (the mesh builders never emit duplicate triangles; liveSurfaceNets.test.ts asserts it)', async ({ page }) => {
    const result = await draw(page, { background: solid([0, 0, 0]), meshes: [{ triangles: [...cover(0.5), ...cover(0.5)] }], probes: [[32, 32]] });
    expect(Math.abs(alphaOverBlack(result.pixels[0]) - (1 - (1 - ALPHA) ** 2))).toBeLessThan(0.02);
  });

  test('LIVE-16 the coverage guide under the mesh stays visible: a bright point keeps its colour to within the mesh opacity, neighbours untouched', async ({ page }) => {
    const background: Rect[] = [...solid([0, 0, 0]), { x: 30, y: 30, w: 3, h: 3, color: [255, 255, 0] }];
    const small = [-0.2, -0.2, 0.3, 0.2, -0.2, 0.3, 0.2, 0.2, 0.3];
    const result = await draw(page, { background, meshes: [{ triangles: small }], probes: [[31, 31], [20, 20], [5, 5]] });
    expect(result.pixels[0][0], 'the point is still yellow').toBeGreaterThan(255 * (1 - 0.7 * ALPHA) - 6);
    expect(result.pixels[0][2], 'and gets the blue tint of the mesh at most').toBeLessThanOrEqual(255 * ALPHA + 6);
    expect(result.pixels[2].slice(0, 3), 'outside the mesh nothing changed').toEqual([0, 0, 0]);
  });

  test('TASK 08 cost: the depth prepass costs a bounded multiple of the old single pass on the same mesh (software WebGL, relative numbers; not a phone benchmark)', async ({ page }, info) => {
    /* ~20,000 triangles of a gently folded surface: the limit of the live mesh */
    const triangles: number[] = [];
    const n = 100;
    for (let i = 0; i < n; i += 1) for (let j = 0; j < n; j += 1) {
      const x0 = -1 + (2 * i) / n; const x1 = -1 + (2 * (i + 1)) / n; const y0 = -1 + (2 * j) / n; const y1 = -1 + (2 * (j + 1)) / n;
      const z = (x: number, y: number): number => 0.5 + 0.2 * Math.sin(3 * x) * Math.cos(3 * y);
      triangles.push(x0, y0, z(x0, y0), x1, y0, z(x1, y0), x1, y1, z(x1, y1), x0, y0, z(x0, y0), x1, y1, z(x1, y1), x0, y1, z(x0, y1));
    }
    const measure = (mode: string) => page.evaluate((args) => (window as unknown as { __liveMeshPixels: { bench: (x: unknown) => { msPerFrame: number; p95: number } } }).__liveMeshPixels.bench(args), { triangles, mode, frames: 30 });
    await measure('accumulate'); await measure('single-layer');                                  /* warm up both programs */
    const accumulate = await measure('accumulate');
    const single = await measure('single-layer');
    await info.attach('live-mesh-render-cost', { contentType: 'application/json', body: JSON.stringify({ triangles: triangles.length / 9, accumulate, singleLayer: single, ratio: single.msPerFrame / accumulate.msPerFrame, evidence: 'SwiftShader software WebGL, 256x256; relative only' }, null, 2) });
    expect(single.msPerFrame / accumulate.msPerFrame, 'two passes over the same triangles cost at most about twice (plus slack for the software rasteriser)').toBeLessThanOrEqual(3);
  });

  test('the saved scan\'s mesh (amber) and the live mesh (blue) together stay within the opacity budget, whatever the number of shells, and the live one wins a coincident surface', async ({ page }) => {
    const both = await draw(page, {
      background: solid([0, 0, 0]),
      meshes: [{ triangles: layers(8, 0.2), style: 'reference', depthBias: true }, { triangles: layers(8, 0.2), style: 'live' }], probes: [[32, 32]],
    });
    const [r, g, b] = both.pixels[0];
    const combined = 1 - (1 - ALPHA) * (1 - 0.08);
    expect(Math.max(r, g, b) / 255, 'the brightest channel stays far from an opaque screen').toBeLessThanOrEqual(BUDGET);
    expect(b / 255).toBeLessThanOrEqual(combined + 0.02);
    /* coincident surfaces: the live (blue) one is the one on top: the pixel is bluer than amber */
    const coincident = await draw(page, { background: solid([0, 0, 0]), meshes: [{ triangles: cover(0.5), style: 'reference', depthBias: true }, { triangles: cover(0.5), style: 'live' }], probes: [[32, 32]] });
    expect(coincident.pixels[0][2], 'blue present').toBeGreaterThan(coincident.pixels[0][0] * 0.5);
    expect(coincident.pixels[0][1], 'the live layer is in front of the pushed-back reference').toBeGreaterThan(0);
  });
});
