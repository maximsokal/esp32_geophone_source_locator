// Phone Lab: serves the real WebUI to a phone over the home Wi-Fi (HTTPS, fake ESP32 API, telemetry) and prints a QR code.
import { request } from 'node:https';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { networkInterfaces } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createServer as createViteServer } from 'vite';
import { pickLanAddress } from './phone-lab/lan.mjs';
import { ensureLeafCert, findOpenssl } from './phone-lab/cert.mjs';
import { phoneLabPlugin } from './phone-lab/plugin.mjs';
import { renderQrLines } from './phone-lab/qr-terminal.mjs';

const PORT = Number(process.env.PHONE_LAB_PORT ?? 5443);
const webuiRoot = fileURLToPath(new URL('..', import.meta.url));
const repoRoot = join(webuiRoot, '..');
const stamp = (d = new Date()) => {
  const p = (n) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}_${p(d.getHours())}${p(d.getMinutes())}${p(d.getSeconds())}`;
};
const say = (line = '') => console.log(line);

const lan = pickLanAddress(networkInterfaces(), process.env.PHONE_LAB_HOST);
if (lan === null) {
  say('[phone-lab] ERROR no private LAN IPv4 address found. Connect the PC to the home Wi-Fi or set PHONE_LAB_HOST.');
  process.exit(1);
}
if (lan.candidates.length > 1) {
  say(`[phone-lab] interfaces: ${lan.candidates.map((c) => `${c.name}=${c.address}`).join(', ')} (using ${lan.address}; override with PHONE_LAB_HOST)`);
}

const runId = stamp();
const runDir = join(repoRoot, 'logs', 'phone-lab', runId);
const labRoot = join(repoRoot, '.test-build', 'phone-lab');
mkdirSync(runDir, { recursive: true });

let cert;
try {
  cert = ensureLeafCert({
    ip: lan.address,
    outDir: join(labRoot, 'certs'),
    caDir: join(repoRoot, 'components', 'web_server', 'certs'),
    openssl: findOpenssl(),
    selfSigned: process.env.PHONE_LAB_SELFSIGNED === '1',
  });
} catch (error) {
  say(`[phone-lab] ERROR certificate: ${error instanceof Error ? error.message : String(error)}`);
  process.exit(1);
}

let lab = null;
const vite = await createViteServer({
  configFile: join(webuiRoot, 'vite.phone-lab.config.ts'),
  root: webuiRoot,
  server: { host: '0.0.0.0', port: PORT, strictPort: true, https: { key: readFileSync(cert.key), cert: readFileSync(cert.cert) } },
  plugins: [phoneLabPlugin({ runDir, runId, storeDir: join(labRoot, 'device-store'), onReady: (l) => { lab = l; } })],
});
await vite.listen();

const url = `https://${lan.address}:${PORT}/`;
writeFileSync(join(runDir, 'run-metadata.json'), JSON.stringify({
  runId, startedAt: new Date().toISOString(), url, lan, certMode: cert.mode, chainVerified: cert.chainVerified,
  certExpires: new Date(cert.notAfterMs).toISOString(), node: process.version,
}, null, 2));

const qr = (await vite.ssrLoadModule('/src/domain/qrCode.ts')).encodeQr(url);
const expires = new Date(cert.notAfterMs).toISOString().slice(0, 10);
say();
say('[phone-lab] ================================================================');
say(`[phone-lab] phone page : ${url}`);
say(`[phone-lab] logs       : ${runDir}`);
say(`[phone-lab] certificate: ${cert.mode}${cert.reused ? ' (reused)' : ' (issued)'}, expires ${expires}${
  cert.mode === 'device-ca'
    ? (cert.chainVerified ? ', chain verified against the GeoLocator Root CA' : '')
    : ' - the phone must be told to trust it (open the page and continue past the warning)'}`);
say('[phone-lab] On the phone (same Wi-Fi): scan the QR below, then open Objects -> a session -> add 3D scan.');
say();
if (qr === null) say('[phone-lab] (URL too long for the built-in QR encoder; type the URL above)');
else for (const line of renderQrLines(qr.modules)) say(line);
say();

await new Promise((resolve) => {
  const probe = request({ host: lan.address, port: PORT, path: '/lab/health', method: 'GET', rejectUnauthorized: false, timeout: 4000 }, (response) => {
    response.resume();
    say(`[phone-lab] self-check via ${lan.address}: HTTP ${response.statusCode} (server is listening on the LAN address)`);
    resolve();
  });
  probe.on('error', (error) => { say(`[phone-lab] WARNING self-check failed: ${error.message}`); resolve(); });
  probe.on('timeout', () => { probe.destroy(); });
  probe.end();
});
say(`[phone-lab] If the phone cannot connect, allow the port once (admin PowerShell): New-NetFirewallRule -DisplayName "GeoLocator Phone Lab" -Direction Inbound -Protocol TCP -LocalPort ${PORT} -Profile Private -Action Allow`);
say('[phone-lab] stop : Ctrl+C / stop the task');

let last = '';
setInterval(() => {
  if (lab === null) return;
  const c = lab.counts;
  const text = `events=${c.events} api=${c.api} clientLog=${c.clientLog} saves=${c.saves} unmodeled=${c.unmodeled}`;
  if (text !== last) { last = text; say(`[phone-lab] ${new Date().toLocaleTimeString()} ${text}`); }
}, 5000);

let stopping = false;
async function stop(reason) {
  if (stopping) return;
  stopping = true;
  try { await vite.close(); } catch { /* closing */ }
  writeFileSync(join(runDir, 'run-metadata.stopped.json'), JSON.stringify({ stoppedAt: new Date().toISOString(), reason, counts: lab?.counts ?? null }, null, 2));
  process.exit(0);
}
for (const signal of ['SIGINT', 'SIGTERM', 'SIGHUP']) process.on(signal, () => void stop(signal));
