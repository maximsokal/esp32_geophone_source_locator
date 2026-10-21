import { defineConfig } from '@playwright/test';
import base from './playwright.config.ts';
import manifest from './tests/e2e/snapshots/manifest.json' with { type: 'json' };
import { visualStates } from './tests/e2e/fixtures/visual-states.ts';

const mode = process.env.UI_VISUAL_MODE ?? 'compare';
const tier = process.env.UI_VISUAL_TIER ?? 'critical';
if (!['capture', 'compare'].includes(mode) || !['critical', 'all'].includes(tier)) throw new Error('Invalid visual mode/selection');
for (const name of ['critical', 'extended']) {
  if (visualStates.filter((s) => s.tier === name).length !== manifest.selection[name]) throw new Error(`Visual selection changed: ${name}`);
}
const selected = visualStates.filter((s) => tier === 'all' || s.tier === 'critical');
export default defineConfig({ ...base, projects: [] }, {
  testMatch: 'visual.spec.ts',
  grep: tier === 'critical' ? /@critical/ : undefined,
  timeout: 45_000,
  updateSnapshots: 'none',
  snapshotPathTemplate: `{testDir}/snapshots/${manifest.environment.id}/{arg}{ext}`,
  expect: { ...base.expect, toHaveScreenshot: { animations: 'disabled', caret: 'hide', scale: 'css', threshold: 0, maxDiffPixels: 0 } },
  reporter: [
    ['line'],
    ['json', { outputFile: '../.test-build/ui-visual/playwright.json' }],
    ['html', { outputFolder: '../.test-build/ui-visual/html', open: 'never' }],
  ],
  metadata: {
    ...base.metadata,
    visualMode: mode,
    selection: `Visual ${tier}: ${selected.length} cases; synthetic HTTP and fixed CPU/memory inputs`,
    expectedTests: selected.length,
    expectedCaseIds: selected.map((s) => s.id),
    canonical: manifest.environment,
    approvedFiles: Object.keys(manifest.approval?.files ?? {}),
    approvalErrors: JSON.parse(process.env.UI_VISUAL_APPROVAL_ERRORS ?? '{}'),
    reproduce: 'cd webui; npm run test:ui:visual',
  },
  projects: [{ name: manifest.environment.id, use: {
    ...base.projects[0].use,
    deviceScaleFactor: 1, locale: manifest.environment.browserLocale, timezoneId: 'UTC',
    reducedMotion: 'reduce', colorScheme: 'dark', visualMode: mode,
  } }],
});
