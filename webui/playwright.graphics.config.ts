import { defineConfig } from '@playwright/test';
import base from './playwright.config';

export default defineConfig({ ...base,
  testMatch: 'graphics-lifecycle.spec.ts',
  timeout: 90_000,
  use: { ...base.use, screenshot: { mode: 'on', fullPage: true },
    launchOptions: { args: ['--enable-webgl', '--use-angle=swiftshader', '--enable-unsafe-swiftshader'] } },
  outputDir: '../.test-build/ui-graphics/results',
  reporter: [['line'], ['json', { outputFile: '../.test-build/ui-graphics/playwright.json' }],
    ['html', { outputFolder: '../.test-build/ui-graphics/html', open: 'never' }]],
  metadata: { ...base.metadata,
    evidence: 'Native WebGL/Three and emitted Workers; synthetic recorded scan/XR session; SwiftShader, no physical ARCore',
    selection: 'Graphics resource lifecycle, camera strip at five phone sizes EN/UK with touch drag, resize/recovery, session end, existing CPU stress and measured warmup/repeats',
    expectedTests: 25, reproduce: 'cd webui; npm run test:ui:graphics' },
  webServer: { ...base.webServer,
    command: 'node node_modules/vite/bin/vite.js build --config vite.graphics.config.ts && node node_modules/vite/bin/vite.js preview --config vite.graphics.config.ts --host 127.0.0.1 --port 4179 --strictPort',
    url: 'http://127.0.0.1:4179/stress/scan-result-ui.html',
  },
});
