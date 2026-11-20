import { execFileSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { describe, expect, it } from 'vitest';
// @ts-expect-error plain .mjs dev scripts without type declarations
import { buildLeafExtensions, ensureLeafCert, findOpenssl } from '../../scripts/phone-lab/cert.mjs';

/* Uses a THROWAWAY CA created in .test-build; the project's real Root/Device CA files are never opened by these tests. */
const base = resolve(import.meta.dirname, '../../../.test-build/phone-lab-cert-test');
let openssl: string | null = null;
try { openssl = findOpenssl(); } catch { openssl = null; }
const maybe = openssl === null ? describe.skip : describe;
let n = 0;

function makeCa(existing?: string): string {
  const dir = existing ?? join(base, `ca-${process.pid}-${n += 1}`);
  mkdirSync(dir, { recursive: true });
  const run = (...args: string[]) => execFileSync(openssl!, args, { stdio: 'pipe' });
  run('req', '-x509', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:prime256v1', '-nodes', '-keyout', join(dir, 'rootCA.key'), '-out', join(dir, 'rootCA.crt'), '-subj', '/CN=Test Root', '-days', '30', '-addext', 'basicConstraints=critical,CA:TRUE', '-addext', 'keyUsage=critical,keyCertSign,cRLSign');
  writeFileSync(join(dir, 'ext.cnf'), 'basicConstraints = critical, CA:TRUE, pathlen:0\nkeyUsage = critical, keyCertSign, cRLSign\nsubjectKeyIdentifier = hash\nauthorityKeyIdentifier = keyid,issuer\nnameConstraints = @nc\n\n[nc]\npermitted;DNS.0 = geolocator.local\npermitted;IP.0 = 192.168.0.0/255.255.0.0\npermitted;IP.1 = 10.0.0.0/255.0.0.0\n');
  run('req', '-new', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:prime256v1', '-nodes', '-keyout', join(dir, 'deviceCA.key'), '-out', join(dir, 'dev.csr'), '-subj', '/CN=Test Device CA');
  run('x509', '-req', '-in', join(dir, 'dev.csr'), '-CA', join(dir, 'rootCA.crt'), '-CAkey', join(dir, 'rootCA.key'), '-CAcreateserial', '-CAserial', join(dir, 'root.srl'), '-out', join(dir, 'deviceCA.crt'), '-days', '30', '-extfile', join(dir, 'ext.cnf'));
  return dir;
}

describe('leaf extensions', () => {
  it('names only the LAN address (a 127.0.0.1 SAN would violate the Device CA name constraints)', () => {
    const text = buildLeafExtensions('192.168.0.148');
    expect(text).toContain('subjectAltName = IP:192.168.0.148');
    expect(text).not.toContain('127.0.0.1');
    expect(text).toContain('CA:FALSE');
    expect(text).toContain('serverAuth');
  });
});

maybe('phone-lab certificate issuing (throwaway CA)', () => {
  it('signs a leaf with the Device CA, verifies the chain, and reuses it for the same address', () => {
    const caDir = makeCa();
    const outDir = join(base, `out-${process.pid}-${n += 1}`);
    const first = ensureLeafCert({ ip: '192.168.0.148', outDir, caDir, openssl });
    expect(first).toMatchObject({ mode: 'device-ca', reused: false, chainVerified: true });
    const pem = readFileSync(first.cert, 'utf8');
    expect((pem.match(/BEGIN CERTIFICATE/g) ?? []).length).toBe(2); // leaf + device CA, as the phone needs the intermediate
    const san = execFileSync(openssl!, ['x509', '-in', first.cert, '-noout', '-ext', 'subjectAltName'], { encoding: 'utf8' });
    expect(san).toContain('192.168.0.148');
    expect(san).not.toContain('127.0.0.1');
    expect(ensureLeafCert({ ip: '192.168.0.148', outDir, caDir, openssl })).toMatchObject({ reused: true });
    expect(ensureLeafCert({ ip: '192.168.0.149', outDir, caDir, openssl })).toMatchObject({ reused: false, chainVerified: true });
  });

  it('does not reuse a leaf that was signed by a CA which has since been replaced', () => {
    const caDir = makeCa();
    const outDir = join(base, `out-${process.pid}-${n += 1}`);
    expect(ensureLeafCert({ ip: '192.168.0.148', outDir, caDir, openssl })).toMatchObject({ reused: false });
    expect(ensureLeafCert({ ip: '192.168.0.148', outDir, caDir, openssl })).toMatchObject({ reused: true });
    makeCa(caDir); // a regenerated Device CA at the same location
    const after = ensureLeafCert({ ip: '192.168.0.148', outDir, caDir, openssl });
    expect(after).toMatchObject({ reused: false, chainVerified: true });
  });

  it('refuses an address outside the CA name constraints instead of serving a chain Android would reject', () => {
    const caDir = makeCa();
    expect(() => ensureLeafCert({ ip: '8.8.8.8', outDir: join(base, `out-${process.pid}-${n += 1}`), caDir, openssl })).toThrow();
  });

  it('falls back to a self-signed certificate when asked or when no CA is present', () => {
    const outDir = join(base, `out-${process.pid}-${n += 1}`);
    const emptyCa = join(base, `empty-${process.pid}-${n += 1}`);
    mkdirSync(emptyCa, { recursive: true });
    const result = ensureLeafCert({ ip: '192.168.0.148', outDir, caDir: emptyCa, openssl });
    expect(result.mode).toBe('self-signed');
    expect(existsSync(result.key)).toBe(true);
    const forced = ensureLeafCert({ ip: '192.168.0.148', outDir: join(base, `out-${process.pid}-${n += 1}`), caDir: makeCa(), openssl, selfSigned: true });
    expect(forced.mode).toBe('self-signed');
  });
});
