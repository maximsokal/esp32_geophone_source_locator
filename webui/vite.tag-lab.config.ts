import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

/* Development-only Tag Lab page. Never part of the ESP32 bundle: the production vite.config.ts only builds index.html. */
export default defineConfig({
  base: './',
  plugins: [react()],
  appType: 'mpa',
  /* The synthetic source reads the printable-pattern JSON in ../tools, one level above webui. */
  server: { host: '127.0.0.1', port: 5188, strictPort: true, fs: { allow: ['..'] } },
});
