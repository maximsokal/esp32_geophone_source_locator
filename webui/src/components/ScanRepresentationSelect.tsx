import { useT } from '../i18n';

export type ScanRepresentation = 'mesh' | 'esp32-preview' | 'esp32-solver';

/** `absent`: no voxels to offer; `preparing`: the save is under way and the bytes are not built yet; `ready`: the exact bytes exist. */
export type Esp32PreviewAvailability = 'absent' | 'preparing' | 'ready';

/**
 * The existing Mesh entry of the post-scan mode row, as a native select (issue #44): Mesh, the voxels the ESP32 TFT receives, or the solver voxels the
 * localisation receives. It is the ONLY
 * entry point for the 3D representation; the other three modes stay buttons. While another mode is active it shows the neutral "Mesh" caption, so
 * choosing Mesh from there is still a change; once Mesh or the voxels are chosen the selection stays readable in the control itself.
 */
export function ScanRepresentationSelect({ representation, meshActive, preview, solver = 'absent', onChoose }: {
  representation: ScanRepresentation;
  meshActive: boolean;
  preview: Esp32PreviewAvailability;
  solver?: Esp32PreviewAvailability;
  onChoose: (choice: ScanRepresentation) => void;
}) {
  const t = useT();
  const value = representation !== 'mesh' ? representation : meshActive ? 'mesh' : '';
  return (
    <select
      className="scan-mode-select"
      aria-label={t('scan.representation.label')}
      data-active={value === '' ? 'false' : 'true'}
      value={value}
      onChange={(event) => {
        if (event.target.value === 'esp32-preview' || event.target.value === 'esp32-solver' || event.target.value === 'mesh') onChoose(event.target.value);
      }}
    >
      <option value="" hidden>{t('scan.mode.mesh')}</option>
      <option value="mesh">{t('scan.mode.mesh')}</option>
      {preview !== 'absent' && (
        <option value="esp32-preview" disabled={preview === 'preparing'}>
          {t(preview === 'preparing' ? 'scan.representation.esp32_preview_preparing' : 'scan.representation.esp32_preview')}
        </option>
      )}
      {solver !== 'absent' && (
        <option value="esp32-solver" disabled={solver === 'preparing'}>
          {t(solver === 'preparing' ? 'scan.representation.esp32_solver_preparing' : 'scan.representation.esp32_solver')}
        </option>
      )}
    </select>
  );
}
