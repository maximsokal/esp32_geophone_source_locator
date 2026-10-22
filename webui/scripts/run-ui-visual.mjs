import { createHash } from 'node:crypto';
import { readFile, writeFile, readdir, mkdir, realpath, copyFile, rename, lstat } from 'node:fs/promises';
import { resolve, dirname, relative, sep, join, extname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { spawnSync } from 'node:child_process';

const webui = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const manifestPath = (root) => join(root, 'tests/e2e/snapshots/manifest.json');
const json = async (path) => JSON.parse(await readFile(path, 'utf8'));
const sha = (bytes) => createHash('sha256').update(bytes).digest('hex');
const fingerprint = (value) => sha(JSON.stringify(value));
const save = (path, value) => writeFile(path, JSON.stringify(value, null, 2) + '\n');

async function files(directory) {
  let entries;
  try { entries = await readdir(directory, { withFileTypes: true }); } catch (e) { if (e.code === 'ENOENT') return []; throw e; }
  const result = [];
  for (const entry of entries.sort((a, b) => a.name.localeCompare(b.name, 'en'))) {
    const path = join(directory, entry.name);
    if (entry.isDirectory()) result.push(...await files(path));
    else result.push(path);
  }
  return result;
}

export async function sourceInputs(root) {
  const paths = [...await files(join(root, 'src')), ...await files(join(root, 'public')), ...await files(join(root, 'tests/e2e')),
    ...await files(join(root, 'tests/console/fixtures')), ...await files(join(root, 'tests/fixtures')),
    ...await files(join(root, 'scripts'))];
  for (const name of ['index.html', 'package.json', 'package-lock.json', 'vite.config.ts', 'tsconfig.json', 'tsconfig.e2e.json',
    'playwright.config.ts', 'playwright.layout.config.ts', 'playwright.network.config.ts', 'playwright.firmware-pages.config.ts', 'playwright.visual.config.mjs', '../tests/ui_contract.json']) {
    paths.push(resolve(root, name));
  }
  const result = {};
  for (const path of [...new Set(paths)].sort()) {
    const key = relative(root, path).split(sep).join('/');
    if (key.startsWith('tests/e2e/snapshots/')) continue;
    try {
      const bytes = await readFile(path);
      const text = ['.ts', '.tsx', '.mts', '.js', '.mjs', '.css', '.html', '.json', '.svg', '.txt', '.glsl'].includes(extname(path));
      result[key] = sha(text ? bytes.toString('utf8').replaceAll('\r\n', '\n') : bytes);
    } catch (e) { if (e.code !== 'ENOENT') throw e; }
  }
  return Object.fromEntries(Object.entries(result).sort(([a], [b]) => a < b ? -1 : a > b ? 1 : 0));
}

export async function environment(root) {
  const os = Object.fromEntries((await readFile('/etc/os-release', 'utf8')).split('\n')
    .filter((line) => line.includes('=')).map((line) => { const i = line.indexOf('='); return [line.slice(0, i), line.slice(i + 1).replace(/^"|"$/g, '')]; }));
  const fontInputs = {};
  for (const path of [...await files('/usr/share/fonts'), ...await files('/usr/local/share/fonts'), ...await files('/etc/fonts')].sort()) {
    fontInputs[path] = sha(await readFile(path));
  }
  return { platform: process.platform, arch: process.arch, node: process.version,
    os: os.ID, osVersion: os.VERSION_ID, fontsSha256: fingerprint(fontInputs),
    playwright: (await json(join(root, 'node_modules/@playwright/test/package.json'))).version,
    image: process.env.UI_VISUAL_IMAGE ?? null };
}

export function checkEnvironment(expected, actual) {
  for (const key of ['platform', 'arch', 'node', 'os', 'osVersion', 'fontsSha256', 'playwright', 'image']) {
    if (!actual[key] || expected[key] !== actual[key]) throw new Error(`Visual environment mismatch: ${key}; expected=${expected[key]} actual=${actual[key]}`);
  }
}

export async function baselineErrors(root, manifest) {
  if (!manifest.approval) return { '*': 'No explicitly approved visual reference set' };
  const errors = {};
  for (const [name, record] of Object.entries(manifest.approval.files)) {
    if (!/^(en|uk)-[a-z0-9-]+-\d+x\d+\.png$/.test(name)) throw new Error('Invalid approved reference name');
    try {
      const bytes = await readFile(join(root, 'tests/e2e/snapshots', manifest.environment.id, name));
      if (sha(bytes) !== record.sha256) errors[name] = 'Approved reference hash changed';
    } catch (e) { if (e.code !== 'ENOENT') throw e; errors[name] = 'Approved reference is missing'; }
  }
  return errors;
}

function referenceDirectory(root, manifest) {
  if (!/^[a-z0-9][a-z0-9.-]*$/.test(manifest.environment.id) || manifest.environment.id.includes('..')) throw new Error('Invalid reference environment directory');
  return join(root, 'tests/e2e/snapshots', manifest.environment.id);
}

async function referenceDigest(root) {
  const result = {};
  const directory = join(root, 'tests/e2e/snapshots');
  for (const path of await files(directory)) result[relative(directory, path).split(sep).join('/')] = sha(await readFile(path));
  return fingerprint(result);
}

async function inside(directory, name) {
  const root = await realpath(directory);
  const path = await realpath(resolve(root, name));
  if (!path.startsWith(root + sep)) throw new Error('Candidate image escapes its directory');
  return path;
}

export async function verifiedCandidate(directory) {
  const candidate = await json(join(directory, 'candidate.json'));
  const { candidateId, ...body } = candidate;
  if (candidate.schema !== 1 || candidate.status !== 'CAPTURED_NOT_APPROVED' || fingerprint(body) !== candidateId || !Array.isArray(candidate.cases) || !candidate.cases.length) throw new Error('Candidate identity is invalid');
  const names = new Set();
  for (const item of candidate.cases) {
    if (!/^(en|uk)-[a-z0-9-]+-\d+x\d+\.png$/.test(item.file) || names.has(item.file)) throw new Error('Invalid/duplicate candidate filename');
    names.add(item.file);
    const bytes = await readFile(await inside(directory, item.image));
    if (sha(bytes) !== item.sha256 || bytes.length < 33 || bytes.subarray(0, 8).toString('hex') !== '89504e470d0a1a0a' ||
        bytes.subarray(12, 16).toString() !== 'IHDR' || bytes.readUInt32BE(16) !== item.state.width || bytes.readUInt32BE(20) < item.state.height) throw new Error('Candidate PNG changed or is invalid');
  }
  return candidate;
}

export async function stability(firstDir, secondDir) {
  const a = await verifiedCandidate(firstDir), b = await verifiedCandidate(secondDir);
  if (fingerprint(a.environment) !== fingerprint(b.environment) || fingerprint(a.inputs) !== fingerprint(b.inputs)) throw new Error('Stability runs have different source/environment');
  const before = Object.fromEntries(a.cases.map((c) => [c.file, c.sha256]));
  const after = Object.fromEntries(b.cases.map((c) => [c.file, c.sha256]));
  const changed = [...new Set([...Object.keys(before), ...Object.keys(after)])].filter((key) => before[key] !== after[key]);
  const result = { status: changed.length ? 'UNSTABLE' : 'STABLE', first: a.candidateId, second: b.candidateId,
    secondDirectory: relative(resolve(firstDir), resolve(secondDir)).split(sep).join('/'), cases: a.cases.length, changed };
  await save(join(firstDir, 'stability.json'), result);
  if (changed.length) throw new Error(`Visual captures are unstable: ${changed.join(', ')}`);
  return result;
}

export async function approveCandidate(root, directory, candidateId, selected) {
  const candidate = await verifiedCandidate(directory);
  if (candidateId !== candidate.candidateId) throw new Error('Explicit reviewed candidate ID is required');
  const stable = await json(join(directory, 'stability.json'));
  if (stable.status !== 'STABLE' || stable.first !== candidateId) throw new Error('Stable repeated capture is required');
  await stability(directory, resolve(directory, stable.secondDirectory));
  const manifest = await json(manifestPath(root));
  if (fingerprint(candidate.canonical) !== fingerprint(manifest.environment)) throw new Error('Candidate belongs to another canonical environment');
  if (fingerprint(candidate.inputs) !== fingerprint(await sourceInputs(root))) throw new Error('Source changed since the reviewed capture');
  const cases = selected === 'all' ? candidate.cases : candidate.cases.filter((c) => selected.includes(c.file));
  if (!cases.length || (selected !== 'all' && (new Set(selected).size !== selected.length || cases.length !== selected.length))) throw new Error('Select exact reviewed filenames or --all');
  const destination = referenceDirectory(root, manifest);
  await mkdir(destination, { recursive: true });
  if (!(await realpath(destination)).startsWith(await realpath(join(root, 'tests/e2e/snapshots')) + sep)) throw new Error('Reference directory escapes snapshots');
  const approved = { ...(manifest.approval?.files ?? {}) };
  for (const item of cases) {
    try { if ((await lstat(join(destination, item.file))).isSymbolicLink()) throw new Error('Reference destination is a symlink'); }
    catch (e) { if (e.code !== 'ENOENT') throw e; }
    approved[item.file] = { sha256: item.sha256, candidateId };
  }
  manifest.approval = { files: approved };
  const temporary = manifestPath(root) + '.pending';
  await writeFile(temporary, JSON.stringify(manifest, null, 2) + '\n', { flag: 'wx' });
  for (const item of cases) await copyFile(await inside(directory, item.image), join(destination, item.file));
  await rename(temporary, manifestPath(root));
  return cases.length;
}

async function captureRun(mode, tier, output) {
  const manifest = await json(manifestPath(webui));
  if (process.platform !== 'linux') throw new Error('Canonical visual execution requires the pinned Linux image; Windows is a different rendering environment');
  const runtime = await environment(webui);
  checkEnvironment(manifest.environment, runtime);
  const inputs = await sourceInputs(webui);
  const referencesBefore = await referenceDigest(webui);
  await mkdir(dirname(output), { recursive: true });
  await mkdir(output);
  await save(join(output, 'environment.json'), { canonical: manifest.environment, runtime, inputs });
  const result = spawnSync(process.execPath, ['node_modules/@playwright/test/cli.js', 'test', '--config', 'playwright.visual.config.mjs', '--output', join(output, 'results')], {
    cwd: webui, stdio: 'inherit', env: { ...process.env, UI_VISUAL_MODE: mode, UI_VISUAL_TIER: tier,
      UI_VISUAL_APPROVAL_ERRORS: JSON.stringify(await baselineErrors(webui, manifest)),
      PLAYWRIGHT_JSON_OUTPUT_FILE: join(output, 'playwright.json'), PLAYWRIGHT_HTML_OUTPUT_DIR: join(output, 'html') },
  });
  const exitCode = result.status ?? 1;
  const referencesAfter = await referenceDigest(webui);
  await save(join(output, 'execution.json'), { mode, tier, exitCode, referencesBefore, referencesAfter, error: result.error?.message ?? null });
  if (referencesBefore !== referencesAfter) throw new Error('Ordinary visual execution changed reference files');
  if (fingerprint(inputs) !== fingerprint(await sourceInputs(webui))) throw new Error('Visual source changed during execution');
  if (exitCode) throw new Error(`Visual ${mode} process exited ${exitCode}; see ${output}`);
  if (mode !== 'capture') return;
  const report = await json(join(output, 'playwright.json'));
  const cases = [];
  function visit(suites) {
    for (const suite of suites) {
      for (const spec of suite.specs ?? []) for (const test of spec.tests) {
        if (test.status !== 'expected' || test.expectedStatus !== 'passed' || test.results.length !== 1 || test.results[0].status !== 'passed') throw new Error('Incomplete/non-passing candidate run');
        const attachments = test.results[0].attachments;
        const state = JSON.parse(Buffer.from(attachments.find((a) => a.name === 'visual-case').body, 'base64'));
        const path = attachments.find((a) => a.name === 'visual-actual').path;
        cases.push({ id: state.id, file: state.file, image: relative(output, path).split(sep).join('/'), state });
      }
      visit(suite.suites ?? []);
    }
  }
  visit(report.suites);
  const expected = report.config.metadata.expectedCaseIds;
  if (cases.length !== expected.length || new Set(cases.map((c) => c.id)).size !== expected.length || cases.some((c) => !expected.includes(c.id))) throw new Error('Candidate selection is incomplete');
  for (const item of cases) item.sha256 = sha(await readFile(await inside(output, item.image)));
  const body = { schema: 1, status: 'CAPTURED_NOT_APPROVED', sourceHead: process.env.UI_VISUAL_SOURCE_HEAD ?? process.env.GITHUB_SHA ?? null,
    canonical: manifest.environment, environment: runtime, inputs, cases: cases.sort((a, b) => a.file.localeCompare(b.file, 'en')) };
  await save(join(output, 'candidate.json'), { ...body, candidateId: fingerprint(body) });
  console.log(`VISUAL_CANDIDATE: ${cases.length} captured; NOT APPROVED; ${output}`);
}

async function main() {
  const [command, ...args] = process.argv.slice(2);
  if (command === 'probe') { console.log(JSON.stringify(await environment(webui), null, 2)); return; }
  if (command === 'stability' && args.length === 2) { console.log(await stability(...args)); return; }
  if (command === 'approve') {
    const [directory, id, ...selected] = args;
    console.log('Approved files:', await approveCandidate(webui, directory, id, selected.length === 1 && selected[0] === '--all' ? 'all' : selected)); return;
  }
  if (!['capture', 'compare'].includes(command)) throw new Error('Use capture|compare [--extended] [--output directory], stability first second, or approve directory reviewed-id filenames|--all');
  let tier = command === 'capture' ? 'all' : 'critical';
  let output = resolve(webui, '../.test-build/ui-visual', `${new Date().toISOString().replace(/[:.]/g, '-')}-${command}`);
  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--extended') tier = 'all';
    else if (args[i] === '--output' && args[i + 1]) output = resolve(args[++i]);
    else throw new Error(`Unsupported visual argument: ${args[i]}`);
  }
  await captureRun(command, tier, output);
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  main().catch((error) => { console.error(error.message); process.exitCode = 1; });
}
