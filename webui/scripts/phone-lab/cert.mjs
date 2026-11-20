// Issues the HTTPS certificate the phone-lab dev server presents. The CA key is only ever passed to openssl by path;
// this code never opens, prints or copies it.
import { execFileSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { join } from 'node:path';

export const LEAF_DAYS = 30;
const REUSE_MARGIN_DAYS = 3;

export function findOpenssl(env = process.env) {
  if (env.PHONE_LAB_OPENSSL) return env.PHONE_LAB_OPENSSL;
  const candidates = [
    'openssl',
    'C:\\Program Files\\Git\\usr\\bin\\openssl.exe',
    'C:\\Program Files\\Git\\mingw64\\bin\\openssl.exe',
    'C:\\Program Files\\OpenSSL-Win64\\bin\\openssl.exe',
  ];
  for (const candidate of candidates) {
    try { execFileSync(candidate, ['version'], { stdio: 'ignore' }); return candidate; } catch { /* try next */ }
  }
  throw new Error('openssl was not found (set PHONE_LAB_OPENSSL to its path).');
}

/**
 * The Device CA is name-constrained to geolocator.local and private IPv4 ranges, so the leaf may name ONLY the LAN address
 * (a 127.0.0.1 SAN would violate the constraint and make Android reject the chain).
 */
export function buildLeafExtensions(ip) {
  return [
    'basicConstraints = critical, CA:FALSE',
    'keyUsage = critical, digitalSignature',
    'extendedKeyUsage = serverAuth',
    'subjectKeyIdentifier = hash',
    'authorityKeyIdentifier = keyid,issuer',
    `subjectAltName = IP:${ip}`,
    '',
  ].join('\n');
}

/** SHA-256 of the PUBLIC Device CA certificate; a leaf signed by an older CA must not be reused. */
function caFingerprint(caCrt) {
  return createHash('sha256').update(readFileSync(caCrt)).digest('hex');
}

function run(openssl, args, env) {
  return execFileSync(openssl, args, { stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env, ...env }, encoding: 'utf8' });
}

function notAfter(openssl, certPath) {
  const text = run(openssl, ['x509', '-in', certPath, '-noout', '-enddate']);
  const value = /notAfter=(.+)/.exec(text)?.[1];
  const ms = value === undefined ? Number.NaN : Date.parse(value.trim());
  return Number.isFinite(ms) ? ms : 0;
}

/**
 * mode 'device-ca': leaf signed by the project's Device CA (phones that trust the GeoLocator Root CA accept it as is).
 * mode 'self-signed': fallback; the phone must be told to trust it manually.
 */
export function ensureLeafCert({ ip, outDir, caDir, openssl, selfSigned = false, now = Date.now, env = process.env }) {
  mkdirSync(outDir, { recursive: true });
  const caCrt = join(caDir, 'deviceCA.crt');
  const caKey = join(caDir, 'deviceCA.key');
  const rootCrt = join(caDir, 'rootCA.crt');
  const useCa = !selfSigned && existsSync(caCrt) && existsSync(caKey);
  const mode = useCa ? 'device-ca' : 'self-signed';
  const keyPath = join(outDir, 'leaf.key');
  const certPath = join(outDir, 'leaf-chain.pem');
  const metaPath = join(outDir, 'leaf-meta.json');

  if (existsSync(metaPath) && existsSync(keyPath) && existsSync(certPath)) {
    try {
      const meta = JSON.parse(readFileSync(metaPath, 'utf8'));
      const sameCa = !useCa || meta.caSha256 === caFingerprint(caCrt);
      if (meta.ip === ip && meta.mode === mode && sameCa && meta.notAfterMs - now() > REUSE_MARGIN_DAYS * 86400000) {
        return { key: keyPath, cert: certPath, mode, reused: true, ip, notAfterMs: meta.notAfterMs, chainVerified: meta.chainVerified };
      }
    } catch { /* reissue */ }
  }

  // OpenSSL checks a leaf's CN against the CA's DNS name constraint when the SAN has no DNS entry, so a CN holding an IP would be
  // "outside geolocator.local". Name the permitted host in the subject and keep the address only in the SAN.
  const subject = useCa ? '/C=UA/O=GeoLocator/CN=geolocator.local' : `/C=UA/O=GeoLocator/CN=${ip}`;
  const ext = join(outDir, 'leaf-ext.cnf');
  writeFileSync(ext, buildLeafExtensions(ip));
  const curve = ['-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:prime256v1'];
  let chainVerified = false;
  if (useCa) {
    const csr = join(outDir, 'leaf.csr');
    const leafCrt = join(outDir, 'leaf.crt');
    run(openssl, ['req', '-new', ...curve, '-nodes', '-keyout', keyPath, '-out', csr, '-subj', subject]);
    const passArgs = env.PHONE_LAB_CA_PASS ? ['-passin', 'env:PHONE_LAB_CA_PASS'] : [];
    try {
      run(openssl, ['x509', '-req', '-in', csr, '-CA', caCrt, '-CAkey', caKey, ...passArgs, '-CAserial', join(outDir, 'ca.srl'), '-CAcreateserial',
        '-out', leafCrt, '-days', String(LEAF_DAYS), '-sha256', '-extfile', ext], env.PHONE_LAB_CA_PASS ? { PHONE_LAB_CA_PASS: env.PHONE_LAB_CA_PASS } : {});
    } catch (error) {
      const text = String(error.stderr ?? error.message);
      throw new Error(`Signing with the Device CA failed (${text.split('\n')[0]}). If the CA key has a passphrase, set PHONE_LAB_CA_PASS; or use PHONE_LAB_SELFSIGNED=1.`);
    }
    writeFileSync(certPath, readFileSync(leafCrt, 'utf8') + readFileSync(caCrt, 'utf8'));
    if (existsSync(rootCrt)) {
      // Fail early if Android would reject this chain (name constraints, validity window, wrong issuer).
      run(openssl, ['verify', '-CAfile', rootCrt, '-untrusted', caCrt, '-verify_ip', ip, leafCrt]);
      chainVerified = true;
    }
  } else {
    run(openssl, ['req', '-x509', ...curve, '-nodes', '-keyout', keyPath, '-out', certPath, '-subj', subject, '-days', String(LEAF_DAYS), '-sha256',
      '-addext', `subjectAltName=IP:${ip},IP:127.0.0.1`]);
  }
  const expires = notAfter(openssl, certPath);
  writeFileSync(metaPath, JSON.stringify({ ip, mode, notAfterMs: expires, chainVerified, caSha256: useCa ? caFingerprint(caCrt) : null }, null, 2));
  return { key: keyPath, cert: certPath, mode, reused: false, ip, notAfterMs: expires, chainVerified };
}
