import { execSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { cpus } from 'node:os';
import { dirname, resolve } from 'node:path';

export function quantile(sorted: number[], q: number): number { return sorted.length === 0 ? 0 : sorted[Math.min(sorted.length - 1, Math.max(0, Math.ceil(q * sorted.length) - 1))]; }
export interface Summary { n: number; mean: number; p50: number; p95: number; p99: number; max: number; sum: number }
export function summarize(samples: number[]): Summary {
  const sorted = samples.slice().sort((a, b) => a - b);
  const sum = samples.reduce((a, b) => a + b, 0);
  return { n: samples.length, mean: samples.length ? sum / samples.length : 0, p50: quantile(sorted, 0.5), p95: quantile(sorted, 0.95), p99: quantile(sorted, 0.99), max: sorted.at(-1) ?? 0, sum };
}
export const median = (values: number[]): number => values.slice().sort((a, b) => a - b)[Math.floor(values.length / 2)] ?? 0;

export const LABEL = process.env.PHASE2_LABEL ?? 'working-tree';
const REPO = resolve(__dirname, '../../../../..');
const REPORT_DIR = resolve(REPO, '.test-build/phase2');

export function commitInfo(): { commit: string; dirty: boolean } {
  try {
    const commit = execSync('git rev-parse --short=8 HEAD', { cwd: REPO, encoding: 'utf8' }).trim();
    const dirty = execSync('git status --porcelain --untracked-files=no', { cwd: REPO, encoding: 'utf8' }).trim().length > 0;
    return { commit, dirty };
  } catch { return { commit: 'unknown', dirty: false }; }
}

/** Merges `section` into <report>/<name>-<label>.json so that several bench files can contribute to one report. */
export function writeReport(name: string, section: string, value: unknown): void {
  const path = resolve(REPORT_DIR, `${name}-${LABEL}.json`);
  mkdirSync(dirname(path), { recursive: true });
  const current = existsSync(path)
    ? JSON.parse(readFileSync(path, 'utf8'))
    : { label: LABEL, ...commitInfo(), machine: { cpu: cpus()[0]?.model.trim(), logicalProcessors: cpus().length, node: process.version, kind: 'DESKTOP (Node), not a phone' }, sections: {} };
  current.sections[section] = value;
  writeFileSync(path, JSON.stringify(current, null, 2));
}

export function heapMb(): number { return process.memoryUsage().heapUsed / 1048576; }
