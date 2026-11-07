/*
 * Web mirror of the 11 base roles in components/ui/ui_theme.h.
 * Values are checked against that header and against theme.css by
 * tests/components/theme.drift.dom.test.tsx.
 */
export const uiTheme = {
  background: '#08111F',
  header: '#0E1A2B',
  panel: '#132238',
  panelAlt: '#1B2D46',
  accent: '#21C7D9',
  good: '#4CD681',
  warning: '#F3B84B',
  error: '#F05A67',
  linkMeasuring: '#4A90E2',
  text: '#F3F7FB',
  muted: '#94A7BC',
} as const;

export type UiThemeRole = keyof typeof uiTheme;

/** Linear mix of two #RRGGBB colours in sRGB; `amount` 0 = a, 1 = b. Used for tone variants of the base roles. */
export function mixHex(a: string, b: string, amount: number): string {
  const channel = (hex: string, offset: number) => parseInt(hex.slice(offset, offset + 2), 16);
  const out = [1, 3, 5].map((offset) => {
    const value = Math.round(channel(a, offset) * (1 - amount) + channel(b, offset) * amount);
    return value.toString(16).padStart(2, '0');
  });
  return `#${out.join('').toUpperCase()}`;
}
