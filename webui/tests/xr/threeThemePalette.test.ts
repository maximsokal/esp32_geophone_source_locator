import { describe, expect, it } from 'vitest';
import { SCAN_LAYER_COLORS } from '../../src/xr/scanViewportScene';
import { uiTheme } from '../../src/theme';
import shaderSource from '../../src/xr/liveCoverageRenderer.ts?raw';
import scanViewportSource from '../../src/components/ScanPointCloudViewport.tsx?raw';
import sceneViewportSource from '../../src/components/SceneViewport.tsx?raw';
import overlaySceneSource from '../../src/features/objects/LocalizationOverlayScene.tsx?raw';

/* S10/S11 (RED_DESIGN): the 3D scan layers use the 11 LVGL roles while the
 * quality / free / unknown / edge layers stay distinguishable from each other. */
const ROLE_VALUES = new Set<string>(Object.values(uiTheme));

describe('3D scan layer colours come from the LVGL palette', () => {
  it('uses only palette colours and keeps every layer role distinct', () => {
    const colours = Object.values(SCAN_LAYER_COLORS);
    for (const colour of colours) expect(ROLE_VALUES.has(colour), colour).toBe(true);
    expect(new Set(colours).size).toBe(colours.length);
  });

  it('maps quality bands to ERROR / WARNING / GOOD', () => {
    expect(SCAN_LAYER_COLORS.weak).toBe(uiTheme.error);
    expect(SCAN_LAYER_COLORS.improving).toBe(uiTheme.warning);
    expect(SCAN_LAYER_COLORS.stable).toBe(uiTheme.good);
  });

  it('live coverage shader uses the same palette (sRGB components, 3 decimals)', () => {
    const vec = (hex: string) => [1, 3, 5].map((i) => (parseInt(hex.slice(i, i + 2), 16) / 255).toFixed(3)).join(', ');
    for (const hex of [uiTheme.error, uiTheme.warning, uiTheme.good, uiTheme.text, uiTheme.accent]) {
      expect(shaderSource, hex).toContain(`vec3(${vec(hex)})`);
    }
  });

  it.each([
    ['ScanPointCloudViewport', scanViewportSource],
    ['SceneViewport', sceneViewportSource],
    ['LocalizationOverlayScene', overlaySceneSource],
  ])('%s has no private hex or rgba colours', (_name, source) => {
    expect(source.match(/#[0-9a-fA-F]{6}\b|rgba?\(/g) ?? []).toEqual([]);
  });
});
