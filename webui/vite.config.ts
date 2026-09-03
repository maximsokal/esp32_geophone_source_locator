import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

export default defineConfig({
  base: './',
  plugins: [react()],
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    sourcemap: false,
    manifest: true,
    target: 'es2022',
    assetsDir: 'assets',
    rolldownOptions: {
      output: {
        entryFileNames: 'assets/app.js',
        chunkFileNames: 'assets/[name]-[hash].js',
        codeSplitting: {
          groups: [
            {
              name: 'initial-vendor',
              test: /node_modules[\/]/,
              tags: ['$initial'],
              maxSize: 320_000,
              priority: 20,
            },
            {
              name: 'lazy-vendor',
              test: /node_modules[\/]/,
              maxSize: 420_000,
              priority: 10,
            },
          ],
        },
      },
    },
  },
});
