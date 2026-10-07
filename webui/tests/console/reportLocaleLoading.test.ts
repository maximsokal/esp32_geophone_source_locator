import { afterEach, expect, it, vi } from 'vitest';
import { readFileSync } from 'node:fs';

const api = vi.hoisted(() => ({
  fetchLocalizationRuns: vi.fn(async () => null),
  loadObjectScanArtifact: vi.fn(async () => null),
}));
const lazyUk = vi.hoisted(() => {
  let resolve!: (module: { uk: Record<string, string> }) => void;
  let markStarted!: () => void;
  const started = new Promise<void>((done) => { markStarted = done; });
  const promise = new Promise<{ uk: Record<string, string> }>((done) => { resolve = done; });
  return {
    promise,
    started,
    markStarted: () => markStarted(),
    resolve: (module: { uk: Record<string, string> }) => resolve(module),
  };
});
vi.mock('../../src/api/deviceApi', async (importOriginal) => ({
  ...await importOriginal<typeof import('../../src/api/deviceApi')>(),
  ...api,
}));
vi.mock('../../src/i18n/catalog.uk.gen', async () => {
  lazyUk.markStarted();
  return await lazyUk.promise;
});

import { openRecordingReport } from '../../src/features/report/reportExport';
import { translate } from '../../src/i18n';
import { ApiRequestPreemptedError } from '../../src/api/deviceApi';
import type { ObjectProfile, RecordingSessionSummary } from '../../src/domain/device';

afterEach(() => vi.unstubAllGlobals());

function ukrainianCatalog(): Record<string, string> {
  const entries: Record<string, string> = {};
  for (const namespace of ['common', 'web', 'report', 'tft']) {
    const source = JSON.parse(readFileSync(new URL(`../../../i18n/${namespace}.json`, import.meta.url), 'utf8')) as {
      entries: Record<string, { uk: string }>;
    };
    for (const [key, value] of Object.entries(source.entries)) entries[key] = value.uk;
  }
  return entries;
}

it('waits for UK strings before writing the exported report', async () => {
  const object = {
    id: 'OBJ_0001', name: 'Bridge pier', active: true, location: { valid: false }, cable_lengths_m: [5, 5, 5, 5],
  } as unknown as ObjectProfile;
  const session = {
    session_id: 'S000007', complete: true, source: 'AD7606', sample_rate_hz: 500, channel_count: 4,
    samples_written: 12000, dropped_frames: 0, recording_ms: 24000,
  } as RecordingSessionSummary;
  const writes: string[] = [];
  const target = { document: { write: (html: string) => writes.push(html), open: vi.fn(), close: vi.fn() } };
  vi.stubGlobal('window', { open: vi.fn(() => target) });

  const exporting = openRecordingReport(object, session, null, 'uk');
  await lazyUk.started;
  expect(writes).toHaveLength(1);

  lazyUk.resolve({ uk: ukrainianCatalog() });
  await exporting;

  const html = writes.at(-1)!;
  for (const key of ['report.coordinate_frames', 'report.location', 'report.recording'] as const) {
    expect(html).toContain(translate('uk', key));
    expect(html).not.toContain(translate('en', key));
  }
  expect(html).toContain(translate('uk', 'report.print_pdf'));
  expect(html).not.toContain(translate('en', 'report.print_pdf'));
});

it('does not export a report with missing localization data after repeated preemption', async () => {
  const object = {
    id: 'OBJ_0001', name: 'Bridge pier', active: true, location: { valid: false }, cable_lengths_m: [5, 5, 5, 5],
  } as unknown as ObjectProfile;
  const session = {
    session_id: 'S000007', complete: true, source: 'AD7606', sample_rate_hz: 500, channel_count: 4,
    samples_written: 12000, dropped_frames: 0, recording_ms: 24000,
  } as RecordingSessionSummary;
  const writes: string[] = [];
  const target = { document: { write: (html: string) => writes.push(html), open: vi.fn(), close: vi.fn() } };
  vi.stubGlobal('window', { open: vi.fn(() => target) });
  api.fetchLocalizationRuns.mockReset().mockRejectedValue(new ApiRequestPreemptedError());

  await expect(openRecordingReport(object, session)).rejects.toBeInstanceOf(ApiRequestPreemptedError);
  expect(api.fetchLocalizationRuns).toHaveBeenCalledTimes(3);
  expect(writes).toHaveLength(1);
});
