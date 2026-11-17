import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, expect, it } from 'vitest';

/*
 * Board 2026-11-17 (three logs): after a scan the 3D result never appeared, "Failed to fetch dynamically imported module
 * .../ScanPointCloudViewport-*.js". The board's HTTPS server takes one client at a time; the viewer chunk was requested while the
 * scan was being saved, the connection was dropped, and a failed import() stays failed for the life of the page, so Retry could
 * not help. The viewer must arrive with the scanner's own chunk, before a scan exists, and nothing may import() it later.
 */
const app = readFileSync(resolve(__dirname, '../../src/App.tsx'), 'utf8');

describe('the scanner brings its result viewer with it', () => {
  it('imports the viewer statically', () => {
    expect(app).toMatch(/^import \{ ScanPointCloudViewport \} from '\.\/components\/ScanPointCloudViewport';$/m);
  });

  it('never fetches the viewer chunk with import() or lazy()', () => {
    expect(app).not.toMatch(/import\(\s*['"]\.\/components\/ScanPointCloudViewport['"]\s*\)/);
    expect(app).not.toMatch(/lazy\(/);
  });
});
