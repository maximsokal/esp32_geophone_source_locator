import { readFileSync } from 'node:fs';

type Pattern = { tag_id: number; modules: number[][] };
const generated = JSON.parse(readFileSync(new URL('../../../../tools/apriltag_sheet/tag_patterns.gen.json', import.meta.url), 'utf8')) as { tags: Pattern[] };

export interface Raster { rgba: Uint8Array; width: number; height: number; left: number; top: number; tagSize: number; }

/** One white-background frame holding a single sheet tag at (left, top) with `cell`-pixel modules. */
export function rasterTag(tagId: number, options: { width?: number; height?: number; left?: number; top?: number; cell?: number } = {}): Raster {
  const pattern = generated.tags.find((tag) => tag.tag_id === tagId);
  if (pattern === undefined) throw new Error(`no pattern for tag ${tagId}`);
  const cell = options.cell ?? 20;
  const width = options.width ?? 400;
  const height = options.height ?? 300;
  const left = options.left ?? 100;
  const top = options.top ?? 60;
  const rgba = new Uint8Array(width * height * 4).fill(255);
  for (let row = 0; row < 8; row += 1) for (let column = 0; column < 8; column += 1) {
    if (!pattern.modules[row][column]) continue;
    for (let y = top + row * cell; y < top + (row + 1) * cell; y += 1) for (let x = left + column * cell; x < left + (column + 1) * cell; x += 1) {
      const offset = (y * width + x) * 4;
      rgba[offset] = 0; rgba[offset + 1] = 0; rgba[offset + 2] = 0;
    }
  }
  return { rgba, width, height, left, top, tagSize: cell * 8 };
}

export function blankFrame(width = 400, height = 300): Uint8Array { return new Uint8Array(width * height * 4).fill(255); }
