import type { LiveMeshMode } from './webxrRuntime';

const KEY = 'geolocator.liveMesh';
export const LIVE_MESH_MODES: readonly LiveMeshMode[] = ['auto', 'on', 'off'];

/** Auto by default: the mesh over the camera only runs while the phone has headroom. A stored choice that is not one of the three is ignored. */
export function readLiveMeshMode(storage: Pick<Storage, 'getItem'> | null = typeof window === 'undefined' ? null : window.localStorage): LiveMeshMode {
  try {
    const stored = storage?.getItem(KEY) ?? null;
    return LIVE_MESH_MODES.includes(stored as LiveMeshMode) ? stored as LiveMeshMode : 'auto';
  } catch { return 'auto'; }
}

export function writeLiveMeshMode(mode: LiveMeshMode, storage: Pick<Storage, 'setItem'> | null = typeof window === 'undefined' ? null : window.localStorage): void {
  try { storage?.setItem(KEY, mode); } catch { /* a private-mode browser keeps the choice for this page only */ }
}
