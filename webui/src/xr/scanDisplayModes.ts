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

export function scanModeLabel(mode: ScanMode | null): string {
  switch (mode) {
    case 'surface': return 'STABLE SURFACE';
    case 'search-volume': return 'SEARCH VOLUME';
    case 'mesh': return 'SURFACE MESH';
    case 'quality': return 'QUALITY MAP';
    default: return 'CUSTOM LAYERS';
  }
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
