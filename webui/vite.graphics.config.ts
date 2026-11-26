import { defineConfig, mergeConfig } from 'vite';
import base from './vite.config';

// Exercise emitted production modules/Workers without adding stress pages to ESP assets.
export default mergeConfig(base, defineConfig({
  build: {
    outDir: '../.test-build/ui-graphics/bundle',
    rolldownOptions: {
      input: ['stress/scan-result-ui.html', 'stress/xr-mobile-stress.html', 'stress/xr-structural-stress.html', 'stress/live-mesh-pixels.html'],
      output: { entryFileNames: 'assets/[name]-[hash].js' },
    },
  },
}));
