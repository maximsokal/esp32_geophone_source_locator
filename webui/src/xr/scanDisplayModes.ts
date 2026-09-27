export interface ScanDisplayOptions {
  goodPoints: boolean;
  badPoints: boolean;
  dimensions: boolean;
  mesh: boolean;
  freeAir: boolean;
  unknownSpace: boolean;
  manualPoints: boolean;
}

export type ScanMode = 'quality' | 'surface' | 'search-volume' | 'mesh';

/**
 * Mesh is deliberately an overlay in the two surface-quality views. The mesh
 * preview already accepts only sufficiently confident promoted points and uses
 * conservative GP3-style triangle gates, so showing it beneath the coloured
 * points gives the operator immediate visual feedback about where the scan is
 * already geometrically connected and where more observations are still
 * needed.
 */
const qualityDisplay: ScanDisplayOptions = {
  goodPoints: true,
  badPoints: true,
  dimensions: true,
  mesh: true,
  freeAir: false,
  unknownSpace: false,
  manualPoints: true,
};

const surfaceDisplay: ScanDisplayOptions = {
  goodPoints: true,
  badPoints: false,
  dimensions: true,
  mesh: true,
  freeAir: false,
  unknownSpace: false,
  manualPoints: true,
};

const searchVolumeDisplay: ScanDisplayOptions = {
  goodPoints: true,
  badPoints: false,
  dimensions: true,
  mesh: false,
  freeAir: true,
  unknownSpace: true,
  manualPoints: true,
};

const meshDisplay: ScanDisplayOptions = {
  goodPoints: false,
  badPoints: false,
  dimensions: true,
  mesh: true,
  freeAir: false,
  unknownSpace: false,
  manualPoints: true,
};

export function displayForScanMode(mode: ScanMode): ScanDisplayOptions {
  const source = mode === 'surface'
    ? surfaceDisplay
    : mode === 'search-volume'
      ? searchVolumeDisplay
      : mode === 'mesh'
        ? meshDisplay
        : qualityDisplay;
  return { ...source };
}

export function scanModeLabel(mode: ScanMode | null, locale: I18nLocale = 'en'): string {
  switch (mode) {
    case 'surface': return translate(locale, 'scan.mode.surface');
    case 'search-volume': return translate(locale, 'scan.mode.search_volume');
    case 'mesh': return translate(locale, 'scan.mode.mesh');
    case 'quality': return translate(locale, 'scan.mode.quality');
    default: return translate(locale, 'scan.mode.custom');
  }
}

export function displayLayersLabel(display: ScanDisplayOptions, locale: I18nLocale = 'en'): string {
  return ([
    display.goodPoints ? translate(locale, 'scan.layer.stable') : '',
    display.badPoints ? translate(locale, 'scan.layer.tentative') : '',
    display.mesh ? translate(locale, 'scan.layer.mesh') : '',
    display.freeAir ? translate(locale, 'scan.layer.free') : '',
    display.unknownSpace ? translate(locale, 'scan.layer.unknown') : '',
    display.manualPoints ? translate(locale, 'scan.layer.pins') : '',
    display.dimensions ? translate(locale, 'scan.layer.dimensions') : '',
  ].filter(Boolean).join(' + ') || translate(locale, 'scan.layer.empty')).toUpperCase();
}

export function displayFingerprint(display: ScanDisplayOptions): string {
  return [
    display.goodPoints ? 'stable' : '',
    display.badPoints ? 'tentative' : '',
    display.mesh ? 'mesh' : '',
    display.freeAir ? 'free' : '',
    display.unknownSpace ? 'unknown' : '',
    display.manualPoints ? 'pins' : '',
    display.dimensions ? 'dims' : '',
  ].filter(Boolean).join('+') || 'empty';
}

export interface ScanViewState {
  mode: ScanMode | null;
  display: ScanDisplayOptions;
}

/** A mode button: the preset's layers, labelled with the preset. */
export function scanViewForMode(mode: ScanMode): ScanViewState {
  return { mode, display: displayForScanMode(mode) };
}

/**
 * A single layer button flips exactly that layer. The result is no longer a
 * preset -- it is labelled CUSTOM LAYERS even if it happens to equal one,
 * so the operator always sees that layers were changed by hand.
 */
export function toggleScanLayer(state: ScanViewState, layer: keyof ScanDisplayOptions): ScanViewState {
  return { mode: null, display: { ...state.display, [layer]: !state.display[layer] } };
}
import type { I18nLocale } from '../i18n';
import { translate } from '../i18n';
