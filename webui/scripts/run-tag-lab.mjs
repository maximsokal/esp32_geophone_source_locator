// Starts the Tag Lab: loopback log receiver + dedicated Vite page, both in this one process, so stopping the
// task (Ctrl+C / terminal kill) closes everything and there are no orphan child processes.
import { fileURLToPath } from 'node:url';
import { join } from 'node:path';
import { createServer as createViteServer } from 'vite';
import { createTagLabLogServer } from './tag-lab-log-server.mjs';

const LAB_PORT = Number(process.env.TAG_LAB_PORT ?? 5188);
const LOG_PORT = Number(process.env.TAG_LAB_LOG_PORT ?? 5189);
const webuiRoot = fileURLToPath(new URL('..', import.meta.url));
const logsRoot = join(webuiRoot, '..', 'logs', 'tag-lab');

const logs = createTagLabLogServer({ logsRoot, port: LOG_PORT });
await logs.listen();

const vite = await createViteServer({
  configFile: join(webuiRoot, 'vite.tag-lab.config.ts'),
  root: webuiRoot,
  server: { host: '127.0.0.1', port: LAB_PORT, strictPort: true, proxy: { '/lab-log': { target: `http://127.0.0.1:${LOG_PORT}`, rewrite: (p) => p.replace(/^\/lab-log/, '') } } },
});
await vite.listen();

const url = `http://127.0.0.1:${LAB_PORT}/tag-lab.html`;
console.log('');
console.log(`[tag-lab] page : ${url}`);
console.log(`[tag-lab] logs : ${logs.runDir}`);
console.log('[tag-lab] stop : Ctrl+C (closes camera page server and the log receiver)');

let stopping = false;
async function stop(reason) {
  if (stopping) return;
  stopping = true;
  logs.serverLog(`stop requested: ${reason}`);
  try { await vite.close(); } catch (error) { logs.serverLog(`vite close failed: ${String(error)}`); }
  await logs.close();
  process.exit(0);
}
process.on('SIGINT', () => void stop('SIGINT'));
process.on('SIGTERM', () => void stop('SIGTERM'));
process.on('SIGHUP', () => void stop('SIGHUP'));
