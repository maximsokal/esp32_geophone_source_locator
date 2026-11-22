/**
 * The post-scan TSDF mesh is a pure function of the scan snapshot and of the reconstruction settings, so it is computed once for them (issue #43:
 * the phone run 2026-11-21 23:39 ran SCAN_TSDF_READY twice after Quality -> Search volume -> Mesh -> Surface -> Mesh, because hiding the Mesh layer
 * dropped the result and showing it again started a second reconstruction of the same scan).
 *
 * One entry is kept: the result for the snapshot object currently on screen. A different snapshot (another scan, a continuation) replaces it, and
 * a different settings key (for example the optional AI refinement being admitted or not) is a miss, never a stale hit.
 */
export class TsdfMeshCache<Snapshot extends object, Mesh> {
  private snapshot: Snapshot | null = null;
  private settingsKey: string | null = null;
  private mesh: Mesh | null = null;

  get(snapshot: Snapshot, settingsKey: string): Mesh | null {
    return this.snapshot === snapshot && this.settingsKey === settingsKey ? this.mesh : null;
  }

  set(snapshot: Snapshot, settingsKey: string, mesh: Mesh): void {
    this.snapshot = snapshot;
    this.settingsKey = settingsKey;
    this.mesh = mesh;
  }

  clear(): void {
    this.snapshot = null;
    this.settingsKey = null;
    this.mesh = null;
  }
}

/** The reconstruction settings that can change the geometry. Worker concurrency changes only how fast it is computed, not what is computed. */
export function tsdfSettingsKey(options: { allowOptionalAi: boolean }): string {
  return options.allowOptionalAi ? 'optional-ai' : 'baseline';
}
