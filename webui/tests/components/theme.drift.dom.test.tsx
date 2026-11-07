import { describe, expect, it } from 'vitest';
import header from '../../../components/ui/ui_theme.h?raw';
import css from '../../src/theme.css?raw';
import { uiTheme, type UiThemeRole } from '../../src/theme';

/* role -> [C macro in ui_theme.h, CSS custom property in theme.css] */
const ROLES: Record<UiThemeRole, [string, string]> = {
  background: ['UI_COLOR_BACKGROUND', '--ui-bg'],
  header: ['UI_COLOR_HEADER', '--ui-header'],
  panel: ['UI_COLOR_PANEL', '--ui-panel'],
  panelAlt: ['UI_COLOR_PANEL_ALT', '--ui-panel-alt'],
  accent: ['UI_COLOR_ACCENT', '--ui-accent'],
  good: ['UI_COLOR_GOOD', '--ui-good'],
  warning: ['UI_COLOR_WARNING', '--ui-warning'],
  error: ['UI_COLOR_ERROR', '--ui-error'],
  linkMeasuring: ['UI_COLOR_LINK_MEASURING', '--ui-link-measuring'],
  text: ['UI_COLOR_TEXT', '--ui-text'],
  muted: ['UI_COLOR_MUTED', '--ui-muted'],
};

function headerColors(header: string): Record<string, string> {
  const out: Record<string, string> = {};
  for (const m of header.matchAll(/^#define\s+(UI_COLOR_\w+)\s+0x([0-9A-Fa-f]{6})\b/gm)) {
    out[m[1]] = `#${m[2].toUpperCase()}`;
  }
  return out;
}

function cssColors(css: string): Record<string, string> {
  const out: Record<string, string> = {};
  for (const m of css.matchAll(/(--ui-[\w-]+)\s*:\s*(#[0-9A-Fa-f]{6})\s*;/g)) {
    out[m[1]] = m[2].toUpperCase();
  }
  return out;
}

const roleNames = Object.keys(ROLES) as UiThemeRole[];

/* Roles whose header value differs from the web value; empty means no drift. */
function drift(headerText: string, cssText: string): string[] {
  const h = headerColors(headerText);
  const c = cssColors(cssText);
  return roleNames.flatMap((role) => {
    const [macro, prop] = ROLES[role];
    const want = h[macro];
    const bad = [
      c[prop] !== want && `${prop}: css=${c[prop]} header=${want}`,
      uiTheme[role].toUpperCase() !== want && `${role}: ts=${uiTheme[role]} header=${want}`,
    ];
    return bad.filter((x): x is string => typeof x === 'string');
  });
}

describe('theme drift: ui_theme.h <-> theme.css <-> theme.ts', () => {
  it('web theme carries exactly the 11 roles of the native header', () => {
    expect(Object.keys(headerColors(header))).toHaveLength(roleNames.length);
    expect(Object.keys(cssColors(css)).sort()).toEqual(roleNames.map((r) => ROLES[r][1]).sort());
    expect(Object.keys(uiTheme).sort()).toEqual([...roleNames].sort());
  });

  it('has no drift against the real header', () => {
    expect(drift(header, css)).toEqual([]);
  });

  it('detects a changed header colour', () => {
    const mutated = header.replace(/(UI_COLOR_HEADER\s+0x)0E1A2B/, (_match: string, prefix: string) => `${prefix}000000`);
    expect(mutated).not.toBe(header);
    expect(drift(mutated, css)).toHaveLength(2);
  });
});
