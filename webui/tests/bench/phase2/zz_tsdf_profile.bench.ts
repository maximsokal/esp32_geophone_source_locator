import { readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { it } from 'vitest';
import { buildTsdfRequestArgs } from '../../../src/components/ScanPointCloudViewport';
import { buildDirectionalTsdfMesh } from '../../../src/xr/directionalTsdf';
import { decodeScanArtifact } from '../../../src/xr/scanArtifact';
/* TEMPORARY profile, deleted after use */
it('profile of the final reconstruction of the real scan', () => {
  const dir = resolve(__dirname, '../../../../logs/phone-lab/2026-11-26_231509/saves');
  const scan = decodeScanArtifact(new Uint8Array(readFileSync(resolve(dir, readdirSync(dir).find((e) => /_scan_\d+\.bin$/.test(e))!))));
  const args = () => buildTsdfRequestArgs(scan as never) as never;
  buildDirectionalTsdfMesh(args()); buildDirectionalTsdfMesh(args());
  (globalThis as any).__tsdf = {};
  const t0 = performance.now(); for (let i = 0; i < 3; i += 1) buildDirectionalTsdfMesh(args()); const total = (performance.now() - t0) / 3;
  const p = (globalThis as any).__tsdf as Record<string, number>;
  writeFileSync(resolve(__dirname, '../../../../.test-build/phase2/tsdf_profile.txt'), JSON.stringify({ totalMs: Math.round(total), perBuild: Object.fromEntries(Object.entries(p).map(([k, v]) => [k, Math.round(v / 3)])) }));
}, 120_000);
