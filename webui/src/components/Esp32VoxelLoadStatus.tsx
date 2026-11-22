import { useT } from '../i18n';
import type { SavedVoxelState } from '../xr/esp32SavedVoxels';

/**
 * The one line shown while voxels are read back from the ESP32 and when they cannot be shown (issue #44): reading, none on the device, another scan's,
 * or unreadable. Nothing is drawn for those states: no substitute is built from the scan.
 */
export function Esp32VoxelLoadStatus({ state }: { state: SavedVoxelState }) {
  const t = useT();
  if (state.phase === 'idle' || state.phase === 'ready') return null;
  const alert = state.phase !== 'loading';
  const text = state.phase === 'loading' ? t('scan.voxels.loading')
    : state.phase === 'missing' ? t('scan.voxels.load_missing')
      : state.phase === 'stale' ? t('scan.voxels.load_stale')
        : t('scan.voxels.load_failed', { error: state.error ?? '' });
  return <div className="xr-runtime-warning scan-voxel-load-status" role={alert ? 'alert' : 'status'} data-voxel-load={state.phase}>{text}</div>;
}
