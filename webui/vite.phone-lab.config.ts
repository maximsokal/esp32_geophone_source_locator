import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

/* Development-only: serves the REAL production app (index.html) to a phone over the LAN. The runner adds the HTTPS options and the
   phone-lab middleware (fake device API + telemetry) at start-up; nothing here affects the ESP32 bundle (vite.config.ts).
   The server is reachable by every device on the LAN, so file serving stays inside webui/ (no `fs.allow: ['..']`: that exposed the
   whole repository through /@fs/) and certificates/keys are denied on top of Vite's defaults. */
export default defineConfig({
  base: './',
  plugins: [react()],
  server: {
    fs: {
      strict: true,
      deny: ['.env', '.env.*', '**/*.{crt,pem,key,srl,csr}', '**/.git/**', '**/certs/**'],
    },
  },
});
