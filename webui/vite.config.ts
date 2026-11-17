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
              /*
               * One lazy vendor file, never split by size. The 420 kB maxSize split it into 13 pieces and the scanner then
               * opened ~15 parallel module requests (board 2026-11-17, 18-19-54): the phone's extra TLS connections purged
               * the one that was serving, handshakes were aborted (-0x7780) and the browser reported "Failed to fetch
               * dynamically imported module" for the whole scanner. Few big files mean few connections.
               */
              name: 'lazy-vendor',
              test: /node_modules[\\/]/,
              priority: 10,
            },
          ],
        },
      },
    },
  },
});
