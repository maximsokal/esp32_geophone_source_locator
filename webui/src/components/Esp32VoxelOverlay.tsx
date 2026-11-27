import { useLocale, useT } from '../i18n';
import { formatNumber } from '../i18n/format';
import { exposedFaceCount, VOXEL_FACE_BITS } from '../xr/esp32VoxelScene';
import type { Esp32VoxelView, VoxelPick } from './Esp32VoxelLayers';

/**
 * The DOM over the viewer while the ESP32 voxels are shown (issue #44): which representation, the device state of exactly these bytes, the counts,
 * what UNKNOWN means for the solver grid, and the cell the operator picked.
 */
/** `part`: the viewer places the mode badge among the top hints and the details among the bottom controls, so they never overlap (TASK 07); both by default. */
export function Esp32VoxelOverlay({ view, picked, part = 'both' }: { view: Esp32VoxelView; picked: VoxelPick | null; part?: 'badge' | 'details' | 'both' }) {
  const { locale } = useLocale();
  const t = useT();
  const meters = (value: number): string => formatNumber(locale, value, { maximumFractionDigits: 3 });
  const name = t(view.kind === 'preview' ? 'scan.representation.esp32_preview' : 'scan.representation.esp32_solver');
  return <>
    {part !== 'details' && <div className="scan-mode-overlay" data-voxel-status={view.status} data-voxel-kind={view.kind}>
      3D · {name} · {t(`scan.voxels.status.${view.status}` as 'scan.voxels.status.prepared')}
    </div>}
    {part !== 'badge' && <div className="scan-voxel-details" role="status" aria-live="polite">
      {view.kind === 'preview'
        ? <div>{t('scan.voxels.summary', { cells: formatNumber(locale, view.artifact.voxelCount), size: meters(view.artifact.voxelSizeMeters), faces: formatNumber(locale, exposedFaceCount(view.artifact)) })}</div>
        : <>
          <div>{t('scan.solver.summary', {
            occupied: formatNumber(locale, view.occupancy.occupied.length / 3),
            free: formatNumber(locale, view.occupancy.free.length / 3),
            size: meters(view.occupancy.header.voxel_size_m),
          })}</div>
          <div>{t('scan.solver.unknown_note')}</div>
        </>}
      {picked !== null && picked.kind === 'preview' && <div>{t('scan.voxels.cell', {
        i: picked.cell.grid[0], j: picked.cell.grid[1], k: picked.cell.grid[2],
        x: meters(picked.cell.center[0]), y: meters(picked.cell.center[1]), z: meters(picked.cell.center[2]),
        faces: VOXEL_FACE_BITS.filter((_, bit) => (picked.cell.faceMask & (1 << bit)) !== 0).join(' '),
      })}</div>}
      {picked !== null && picked.kind === 'solver' && <div>{t('scan.solver.cell', {
        cls: t(picked.cell.cls === 'occupied' ? 'scan.solver.class.occupied' : 'scan.solver.class.free'),
        i: picked.cell.grid[0], j: picked.cell.grid[1], k: picked.cell.grid[2],
        x: meters(picked.cell.center[0]), y: meters(picked.cell.center[1]), z: meters(picked.cell.center[2]),
      })}</div>}
    </div>}
  </>;
}
