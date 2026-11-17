import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, expect, it } from 'vitest';

/*
 * Board 2026-11-17 (18-19-54 log): the lazy vendor code was split by size into 13 files, opening the scanner made the phone request
 * ~15 modules at once, its extra TLS connections purged the serving one and the scanner failed with "Failed to fetch dynamically
 * imported module". The board serves one or two TLS clients, so the lazy vendor code must stay one file.
 */
const config = readFileSync(resolve(__dirname, '../../vite.config.ts'), 'utf8');

describe('the lazy vendor bundle is a single file', () => {
  it('has a lazy-vendor group without a size split', () => {
    const group = config.match(/name: 'lazy-vendor',([\s\S]*?)\n\s*\},/);
    expect(group).not.toBeNull();
    expect(group![1]).not.toMatch(/maxSize|minSize\s*:/);
  });
});
