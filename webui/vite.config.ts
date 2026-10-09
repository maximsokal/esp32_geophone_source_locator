import { defineConfig } from 'vitest/config';
import react from '@vitejs/plugin-react';

export default defineConfig({
  base: './',
  plugins: [react()],
  test: {
    /*
     * Near-limit scan fixtures, 100-cycle lifecycle soaks and Worker round trips
     * exceed the 5 s default when the whole suite runs on a loaded machine.
     */
    testTimeout: 30_000,
    hookTimeout: 30_000,
  },
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    sourcemap: false,
    manifest: true,
    target: 'es2022',
    assetsDir: 'assets',

    /*
     * An ESP32 HTTPS endpoint has a deliberately small TLS connection budget.
     * Browser modulepreload can otherwise create a speculative burst of
     * parallel TLS requests before app.js has even executed. Keep initial
     * loading demand-driven; lazy WebXR/seismic/3D features remain split.
     */
    modulePreload: false,

    rolldownOptions: {
      output: {
        entryFileNames: 'assets/app.js',
        chunkFileNames: 'assets/[name]-[hash].js',
        codeSplitting: {
          groups: [
            {
              /*
               * Keep the core third-party runtime in one initial vendor file.
               * A maxSize here previously split it into several independent
               * initial-vendor requests, which is counterproductive on the
               * ESP32's constrained HTTPS server.
               */
              name: 'initial-vendor',
              test: /node_modules[\\/]/,
              tags: ['$initial'],
              priority: 20,
            },
            {
              name: 'lazy-vendor',
              test: /node_modules[\\/]/,
              maxSize: 420_000,
              priority: 10,
            },
          ],
        },
      },
    },
  },
});
