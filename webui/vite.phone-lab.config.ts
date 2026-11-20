import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

/* Development-only: serves the REAL production app (index.html) to a phone over the LAN. The runner adds the HTTPS options and the
   phone-lab middleware (fake device API + telemetry) at start-up; nothing here affects the ESP32 bundle (vite.config.ts). */
export default defineConfig({
  base: './',
  plugins: [react()],
  server: { fs: { allow: ['..'] } },
});
