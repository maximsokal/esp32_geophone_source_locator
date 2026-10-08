import { afterEach, beforeAll, expect, it, vi } from 'vitest';
import { create as createFont } from 'fontkit';
import { createHash } from 'node:crypto';

const brokenFontUrl = 'data:font/woff2;base64,AA==';
const api = vi.hoisted(() => ({ fetchLocalizationRuns: vi.fn(), loadObjectScanArtifact: vi.fn() }));
vi.mock('../../src/api/deviceApi', async (importOriginal) => ({
  ...await importOriginal<typeof import('../../src/api/deviceApi')>(),
  ...api,
}));
vi.mock('@fontsource/montserrat/files/montserrat-latin-400-normal.woff2?inline', () => ({ default: 'data:font/woff2;base64,AA==' }));
vi.mock('@fontsource/montserrat/files/montserrat-latin-700-normal.woff2?inline', () => ({ default: 'data:font/woff2;base64,AA==' }));
vi.mock('@fontsource/montserrat/files/montserrat-cyrillic-400-normal.woff2?inline', () => ({ default: 'data:font/woff2;base64,AA==' }));
vi.mock('@fontsource/montserrat/files/montserrat-cyrillic-700-normal.woff2?inline', () => ({ default: 'data:font/woff2;base64,AA==' }));
vi.mock('@fontsource/montserrat/files/montserrat-cyrillic-ext-400-normal.woff2?inline', () => ({ default: 'data:font/woff2;base64,AA==' }));
vi.mock('@fontsource/montserrat/files/montserrat-cyrillic-ext-700-normal.woff2?inline', () => ({ default: 'data:font/woff2;base64,AA==' }));

import { openRecordingReport } from '../../src/features/report/reportExport';
import type { LocalizationRun, LocalizationRunListing } from '../../src/api/deviceApi';
import type { ObjectProfile, RecordingSessionSummary } from '../../src/domain/device';
import { loadUkrainianCatalog, translate } from '../../src/i18n';

afterEach(() => {
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
  vi.resetAllMocks();
});

beforeAll(async () => { await loadUkrainianCatalog(); });

const object: ObjectProfile = {
  id: 'OBJ_0001',
  name: 'Bridge pier',
  active: true,
  location: {
    valid: true, latitude: 50.4501, longitude: 30.5234, accuracy_m: 4,
    altitude_valid: true, altitude_m: 181.2, altitude_accuracy_m: 3, captured_at_ms: 1, source: 'phone',
  },
  cable_lengths_m: [5, 5, 5, 5],
};

const session: RecordingSessionSummary = {
  session_id: 'S000007', complete: true, source: 'AD7606', sample_rate_hz: 500, channel_count: 4,
  samples_written: 12000, dropped_frames: 0, recording_ms: 24000,
};

const run: LocalizationRun = {
  schema_version: 5, object_id: object.id, session_id: session.session_id, computed_at_ms: 1700000000123,
  kind: 'individual_method', method_name: 'gauss_newton_tdoa', method_version: 3,
  channel_mask_used: 15, channel_mask_rejected: 0, dimensionality: 'planar_2d', geometry_computed_at_ms: 1699999999000,
  propagation_model_name: 'c', propagation_speed_mps: 300, propagation_speed_uncertainty_mps: 5,
  confidence_score: 0.9, solved: true, source_position: { x_m: 2.25, y_m: 1.5, z_m: -0.5 }, rms_residual_us: 1000,
  estimated_p_velocity_mps: 1234.5,
  velocity_range: { v_min_mps: 150, v_max_mps: 1200, vp_vs_min: 0, vp_vs_max: 0, source: 'user' },
  velocity_status: 'ok',
  uncertainty: {
    std_m: { x_m: 0.12, y_m: 0.23, z_m: 0.34 }, credible_level: 0.9,
    credible_volume_m3: 0.01, credible_area_m2: 0,
    credible_min: { x_m: 2, y_m: 1, z_m: -1 }, credible_max: { x_m: 2.5, y_m: 2, z_m: 0 },
  },
};

const listing: LocalizationRunListing = {
  object_id: object.id,
  session_id: session.session_id,
  geometry: {
    available: true,
    pods: [
      { pod: 0, computed: true, leveled: true, x_mm: 0, y_mm: 0, z_mm: 0 },
      { pod: 1, computed: true, leveled: true, x_mm: 4000, y_mm: 0, z_mm: 0 },
      { pod: 2, computed: true, leveled: true, x_mm: 0, y_mm: 3000, z_mm: 0 },
    ],
  },
  cs_pairs: { available: false, pairs: [] },
  picks: [],
  runs: [run],
};

function makePopup() {
  const writes: string[] = [];
  const fonts = {
    ready: new Promise<never>(() => {}),
    load: vi.fn().mockRejectedValue(new Error('popup font load rejected')),
  };
  return {
    writes,
    fonts,
    target: {
      document: {
        fonts,
        write: (html: string) => writes.push(html),
        open: vi.fn(),
        close: vi.fn(),
      },
    },
  };
}

function withTimeout<T>(promise: Promise<T>): Promise<T> {
  let timer: ReturnType<typeof setTimeout> | undefined;
  return Promise.race([
    promise,
    new Promise<never>((_, reject) => {
      timer = setTimeout(() => reject(new Error('report export waited for popup fonts')), 2000);
    }),
  ]).finally(() => {
    if (timer !== undefined) clearTimeout(timer);
  });
}

function renderedMachineData(html: string): string {
  const attributes = [...html.matchAll(/\s(data-[a-z0-9-]+)="([^"]*)"/g)].map(([, key, value]) => [key, value]);
  const urls = [...html.matchAll(/href="([^"]+)"/g)].map(([, url]) => url);
  const payloads = [...html.matchAll(/data-payload="([^"]+)"/g)].map(([, payload]) => payload);
  const qrPaths = [...html.matchAll(/<path d="([^"]+)"/g)].map(([, path]) => path);
  return JSON.stringify({ attributes, urls, payloads, qrPaths });
}

function hashMachineData(html: string): string {
  return createHash('sha256').update(renderedMachineData(html)).digest('hex');
}

const genericFontFamilies = new Set([
  'serif', 'sans-serif', 'monospace', 'cursive', 'fantasy', 'system-ui', 'ui-serif', 'ui-sans-serif',
  'ui-monospace', 'ui-rounded', 'emoji', 'math', 'fangsong',
]);

function fontFamilyStacks(html: string): string[] {
  const withoutFaceDescriptors = html.replace(/@font-face\s*\{[^}]*\}/gi, '');
  return [...withoutFaceDescriptors.matchAll(/font-family\s*:\s*([^;}"'>]+)/gi)].map(([, stack]) => stack.trim());
}

function assertGenericFontFallback(html: string) {
  const stacks = fontFamilyStacks(html);
  expect(stacks).not.toHaveLength(0);
  for (const stack of stacks) {
    const lastFamily = stack.split(',').at(-1)!.trim().replace(/^['"]|['"]$/g, '').toLowerCase();
    expect(genericFontFamilies.has(lastFamily), `font-family stack "${stack}" lacks a generic fallback`).toBe(true);
  }
  return stacks;
}

function unicodeRanges(html: string): Array<[number, number]> {
  const faces = [...html.matchAll(/@font-face\s*\{([^}]*)\}/gi)].map(([, face]) => face);
  expect(faces).toHaveLength(6);
  const ranges: Array<[number, number]> = [];
  for (const face of faces) {
    const srcDeclarations = [...face.matchAll(/\bsrc\s*:/gi)];
    expect(srcDeclarations).toHaveLength(1);
    const sourceUrls = [...face.matchAll(/url\((['"])(.*?)\1\)/gi)].map(([, , url]) => url);
    expect(sourceUrls).toHaveLength(1);

    const rangeDeclarations = [...face.matchAll(/\bunicode-range\s*:\s*([^;}]+)/gi)];
    expect(rangeDeclarations).toHaveLength(1);
    for (const token of rangeDeclarations[0][1].split(',')) {
      const match = token.trim().match(/^U\+([0-9a-f]{1,6})(?:-([0-9a-f]{1,6}))?$/i);
      expect(match, `invalid unicode-range token: ${token}`).not.toBeNull();
      ranges.push([Number.parseInt(match![1], 16), Number.parseInt(match![2] ?? match![1], 16)]);
    }
  }
  return ranges;
}

function assertBrokenFontAssetsAndCoverage(html: string) {
  const ranges = unicodeRanges(html);
  const faces = [...html.matchAll(/@font-face\s*\{([^}]*)\}/gi)].map(([, face]) => face);
  const sourceUrls = faces.map((face) => {
    return [...face.matchAll(/url\((['"])(.*?)\1\)/gi)].map(([, , url]) => url)[0];
  });
  expect(sourceUrls).toEqual(Array(6).fill(brokenFontUrl));
  for (const source of sourceUrls) {
    expect(() => createFont(Buffer.from(source.slice(source.indexOf(',') + 1), 'base64'))).toThrow();
  }

  const body = html.match(/<body\b[^>]*>([\s\S]*?)<\/body>/i)?.[1] ?? '';
  const text = body.replace(/<[^>]*>/g, ' ');
  const uncovered = [...new Set([...text].filter((character) => {
    if (!/\p{Script=Latin}|\p{Script=Cyrillic}/u.test(character)) return false;
    const codePoint = character.codePointAt(0)!;
    return !ranges.some(([start, end]) => codePoint >= start && codePoint <= end);
  }))].sort((left, right) => left.codePointAt(0)! - right.codePointAt(0)!);
  const uncoveredCodes = uncovered.map((character) => `U+${character.codePointAt(0)!.toString(16).toUpperCase()}`);
  expect(uncoveredCodes, `uncovered Latin/Cyrillic report code points: ${uncoveredCodes.join(', ')}`).toEqual([]);
}

it('exports EN and UK reports without waiting for popup fonts or changing machine data', async () => {
  api.fetchLocalizationRuns.mockResolvedValue(listing);
  api.loadObjectScanArtifact.mockResolvedValue(null);

  const englishPopup = makePopup();
  const ukrainianPopup = makePopup();
  const popups = [englishPopup, ukrainianPopup];
  vi.stubGlobal('window', { open: vi.fn(() => popups.shift()?.target ?? null) });
  vi.stubGlobal('FontFace', class { constructor() { throw new Error('report export must not construct FontFace'); } });
  vi.spyOn(Date, 'now').mockReturnValue(1700000000999);
  const before = JSON.stringify({ object, session, listing, run });

  await withTimeout(openRecordingReport(object, session, null, 'en'));
  await withTimeout(openRecordingReport(object, session, null, 'uk'));

  const englishPlaceholder = englishPopup.writes[0];
  const ukrainianPlaceholder = ukrainianPopup.writes[0];
  const englishDocument = englishPopup.writes.at(-1)!;
  const ukrainianDocument = ukrainianPopup.writes.at(-1)!;
  for (const [locale, placeholder, document] of [
    ['en', englishPlaceholder, englishDocument],
    ['uk', ukrainianPlaceholder, ukrainianDocument],
  ] as const) {
    expect(placeholder).toContain(translate(locale, 'report.preparing'));
    expect(document).toContain(translate(locale, 'report.coordinate_frames'));
    expect(document).toContain(translate(locale, 'report.print_pdf'));
    expect(document).toContain('data-object-id="OBJ_0001"');
    expect(document).toContain('data-session-id="S000007"');
    expect(document).toContain('gauss_newton_tdoa');
    expect(document).toContain('Bridge pier');
    expect(document).toContain('<article class="report"');
    assertGenericFontFallback(placeholder);
    assertGenericFontFallback(document);
    assertBrokenFontAssetsAndCoverage(document);
  }

  expect(englishPopup.writes).toHaveLength(2);
  expect(ukrainianPopup.writes).toHaveLength(2);
  expect(englishPopup.fonts.load).not.toHaveBeenCalled();
  expect(ukrainianPopup.fonts.load).not.toHaveBeenCalled();
  expect(hashMachineData(englishDocument)).toBe(hashMachineData(ukrainianDocument));
  expect(JSON.stringify({ object, session, listing, run })).toBe(before);
  expect(api.fetchLocalizationRuns.mock.calls).toEqual([
    [object.id, session.session_id],
    [object.id, session.session_id],
  ]);
  expect(api.loadObjectScanArtifact.mock.calls).toEqual([
    [object.id, session.session_id],
    [object.id, session.session_id],
  ]);
});
