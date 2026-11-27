import { expect, test, type Page } from '@playwright/test';

/*
 * Framebuffer pixel tests of the live mesh renderer on a framebuffer set up like the XR layer (transparent, premultiplied, cleared to zero). The WebXR AR
 * compositor puts that framebuffer over the camera: shown = pixel.rgb + (1 - pixel.a) x camera. So the pixel must be exactly the mesh's premultiplied
 * value where one OR MANY triangles cover it, and exactly zero elsewhere. Nothing here looks at renderer constants: the values are measured.
 */
type Rgba = [number, number, number, number];
interface RunResult { pixels: Rgba[]; triangles: number[]; before: Record<string, unknown> | null; after: Record<string, unknown> | null; contexts: number; glError: number }

/* premultiplied bytes: colour x alpha, alpha. Live: (0.30, 0.78, 1.0) at 0.10. Saved scan: (1.0, 0.72, 0.28) at 0.08. */
const PIXEL: Record<'live' | 'reference', Rgba> = { live: [8, 20, 26, 26], reference: [20, 15, 6, 20] };
const BUDGET = 0.25;

const quad = (x0: number, y0: number, x1: number, y1: number, z: number): number[] => [x0, y0, z, x1, y0, z, x1, y1, z, x0, y0, z, x1, y1, z, x0, y1, z];
const cover = (z: number): number[] => quad(-1, -1, 1, 1, z);
const layers = (count: number): number[] => Array.from({ length: count }, (_, i) => cover(0.2 + (i * 0.75) / count)).flat();
/** `n` x `n` quads of a gently folded surface over the whole view: 2 n^2 triangles. */
function folded(n: number): number[] {
  const out: number[] = [];
  const z = (x: number, y: number): number => 0.5 + 0.2 * Math.sin(3 * x) * Math.cos(3 * y);
  for (let i = 0; i < n; i += 1) for (let j = 0; j < n; j += 1) {
    const [x0, x1, y0, y1] = [-1 + (2 * i) / n, -1 + (2 * (i + 1)) / n, -1 + (2 * j) / n, -1 + (2 * (j + 1)) / n];
    out.push(x0, y0, z(x0, y0), x1, y0, z(x1, y0), x1, y1, z(x1, y1), x0, y0, z(x0, y0), x1, y1, z(x1, y1), x0, y1, z(x0, y1));
  }
  return out;
}
const CROSSING = [-1, -1, 0.2, 1, -1, 0.8, 1, 1, 0.8, -1, -1, 0.2, 1, 1, 0.8, -1, 1, 0.2, -1, -1, 0.8, 1, -1, 0.2, 1, 1, 0.2, -1, -1, 0.8, 1, 1, 0.2, -1, 1, 0.8];
/** 100 x 100 quads (20,000 triangles) of stacked full-view shells would be 10,000 layers; the folded surface plus 0 extra is exactly the live-mesh limit. */
const SCENES: Array<{ name: string; triangles: () => number[]; count: number }> = [
  { name: 'one shell', triangles: () => cover(0.3), count: 2 },
  ...[2, 4, 8, 40].map((n) => ({ name: `${n} overlapping shells`, triangles: () => layers(n), count: 2 * n })),
  { name: '8 coplanar duplicates', triangles: () => Array.from({ length: 8 }, () => cover(0.5)).flat(), count: 16 },
  { name: 'two surfaces crossing', triangles: () => CROSSING, count: 4 },
  { name: 'a folded surface at the live-mesh limit of 20,000 triangles', triangles: () => folded(100), count: 20_000 },
  { name: '20,000 triangles in 10,000 overlapping shells', triangles: () => layers(10_000), count: 20_000 },
];
const PROBES: Array<[number, number]> = [[5, 5], [32, 32], [20, 44], [44, 20], [60, 60]];

const call = <T>(page: Page, name: 'run' | 'bench', spec: unknown): Promise<T> =>
  page.evaluate(([fn, s]) => (window as unknown as { __liveMeshPixels: Record<string, (x: unknown) => unknown> }).__liveMeshPixels[fn as string](s), [name, spec] as const) as Promise<T>;
const draw = (page: Page, spec: Record<string, unknown>): Promise<RunResult> => call<RunResult>(page, 'run', spec);
const near = (pixel: Rgba, expected: Rgba, label: string): void => { pixel.forEach((value, channel) => expect(Math.abs(value - expected[channel]), `${label}: channel ${channel} is ${value}, expected ${expected[channel]}`).toBeLessThanOrEqual(1)); };
/** What the AR compositor shows over a camera pixel of the given brightness. */
const shown = (pixel: Rgba, camera: number): [number, number, number] => [0, 1, 2].map((c) => Math.min(255, pixel[c] + (1 - pixel[3] / 255) * camera)) as [number, number, number];

test.describe('live mesh on a transparent XR-like framebuffer', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/stress/live-mesh-pixels.html');
    await expect.poll(() => page.evaluate(() => (window as unknown as { __liveMeshPixels?: { ready: boolean } }).__liveMeshPixels?.ready ?? false)).toBe(true);
  });

  for (const style of ['live', 'reference'] as const) for (const scene of SCENES) {
    test(`${style}: ${scene.name} gives every covered pixel exactly one premultiplied layer`, async ({ page }) => {
      const result = await draw(page, { meshes: [{ triangles: scene.triangles(), style }], probes: PROBES });
      expect(result.triangles, 'the scene has the number of triangles it claims').toEqual([scene.count]);
      result.pixels.forEach((pixel, i) => near(pixel, PIXEL[style], `probe ${PROBES[i]}`));
      expect(result.pixels[1][3] / 255, 'alpha of the framebuffer, which is what the compositor uses').toBeLessThanOrEqual(BUDGET);
      expect(result.glError).toBe(0);
    });
  }

  test('outside the triangles the framebuffer stays fully transparent, so the camera is untouched there', async ({ page }) => {
    const result = await draw(page, { meshes: [{ triangles: quad(-0.5, -0.5, 0.5, 0.5, 0.3), style: 'live' }], probes: [[32, 32], [3, 3], [60, 32]] });
    near(result.pixels[0], PIXEL.live, 'inside');
    expect(result.pixels.slice(1)).toEqual([[0, 0, 0, 0], [0, 0, 0, 0]]);
  });

  for (const [order, referenceZ, liveZ] of [['saved scan nearer', 0.3, 0.6], ['live nearer', 0.6, 0.3], ['coincident', 0.5, 0.5]] as const) {
    test(`saved scan (amber) then live (blue), ${order}: the live pixel wins where both are, each keeps its own colour elsewhere, nothing adds up`, async ({ page }) => {
      const result = await draw(page, { meshes: [{ triangles: quad(-1, -1, 0.25, 1, referenceZ), style: 'reference' }, { triangles: quad(-0.25, -1, 1, 1, liveZ), style: 'live' }], probes: [[8, 32], [32, 32], [56, 32]] });
      near(result.pixels[0], PIXEL.reference, 'only the saved scan'); near(result.pixels[1], PIXEL.live, 'both'); near(result.pixels[2], PIXEL.live, 'only live');
    });
  }

  test('over the camera: an AprilTag keeps its contrast and a light wall and a dark object keep their brightness under 40 shells', async ({ page }) => {
    const pixel = (await draw(page, { meshes: [{ triangles: layers(40), style: 'live' }], probes: [[32, 32]] })).pixels[0];
    const white = shown(pixel, 255); const black = shown(pixel, 0);
    expect((white[0] - black[0]) / 255, 'tag contrast').toBeGreaterThanOrEqual(0.89);
    expect(shown(pixel, 235)[0] / 235, 'light wall').toBeGreaterThanOrEqual(0.9);
    expect(Math.max(...shown(pixel, 20)), 'dark object').toBeLessThanOrEqual(45);
  });

  test('depth: real depth already in the buffer still hides the mesh, and the mesh writes none (something farther drawn afterwards still shows)', async ({ page }) => {
    const occluder = quad(-1, -1, 0, 1, 0.3);                                       /* real surface over the left half */
    const hidden = await draw(page, { occluders: [occluder], meshes: [{ triangles: cover(0.6), style: 'live' }], probes: [[16, 32], [48, 32]] });
    expect(hidden.pixels[0]).toEqual([0, 0, 0, 0]);
    near(hidden.pixels[1], PIXEL.live, 'nothing in front');
    const later = await draw(page, { meshes: [{ triangles: cover(0.2), style: 'live' }], behind: cover(0.9), probes: [[32, 32]] });
    expect(later.pixels[0], 'the farther red triangle is not occluded by the mesh').toEqual([255, 0, 0, 255]);
  });

  test('the coverage guide is drawn over the meshes and stays visible', async ({ page }) => {
    const without = await draw(page, { meshes: [], guide: [0, 0, 0.5], probes: [[32, 32]] });
    const over = await draw(page, { meshes: [{ triangles: layers(8), style: 'live' }], guide: [0, 0, 0.5], probes: [[32, 32], [5, 5]] });
    expect(without.pixels[0][3], 'the point itself is drawn').toBeGreaterThan(60);
    expect(Math.max(...over.pixels[0].slice(0, 3)), 'and is still brighter than the mesh around it').toBeGreaterThan(Math.max(...PIXEL.live.slice(0, 3)) + 40);
    near(over.pixels[1], PIXEL.live, 'away from the point');
  });

  test('under HIGH frame pressure the mesh is still drawn in EVERY frame (the framebuffer is cleared each frame, so a skipped frame would flicker)', async ({ page }) => {
    for (const frames of [2, 3]) near((await draw(page, { meshes: [{ triangles: cover(0.5), style: 'live' }], pressuredFrames: frames, probes: [[32, 32]] })).pixels[0], PIXEL.live, `frame ${frames} of back-to-back frames`);
  });

  test('WebGL state is left exactly as found, and forty create-draw-dispose cycles keep one context, no GL error and the same picture', async ({ page }) => {
    const guarded = await draw(page, { meshes: [{ triangles: layers(3), style: 'live' }], probes: [[32, 32]], preState: true });
    expect(guarded.after).toEqual(guarded.before);
    near(guarded.pixels[0], PIXEL.live, 'drawn correctly whatever state it found');
    let last = guarded;
    for (let i = 0; i < 40; i += 1) last = await draw(page, { meshes: [{ triangles: layers(3), style: 'live' }], probes: [[32, 32]] });
    near(last.pixels[0], PIXEL.live, 'after forty cycles');
    expect([last.contexts, last.glError]).toEqual([1, 0]);
  });

  test('cost at the 20,000-triangle limit (software WebGL, relative numbers, not a phone benchmark)', async ({ page }, info) => {
    await call(page, 'bench', { triangles: folded(100), frames: 30 });               /* warm up */
    const cost = await call<{ triangles: number; msPerFrame: number; p95: number }>(page, 'bench', { triangles: folded(100), frames: 30 });
    expect(cost.triangles).toBe(20_000);
    await info.attach('live-mesh-render-cost', { contentType: 'application/json', body: JSON.stringify(cost) });
  });
});
