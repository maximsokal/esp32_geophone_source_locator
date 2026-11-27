/**
 * The post-scan TSDF mesh is a pure function of the geometry fed to the reconstruction and of the reconstruction settings, so it is computed once for
 * them (issue #43: the phone run 2026-11-21 23:39 ran SCAN_TSDF_READY twice after Quality -> Search volume -> Mesh -> Surface -> Mesh; the run of
 * 2026-11-26 ran it THREE times with identical metrics, once per viewer component: each viewport held its own cache keyed by the snapshot OBJECT, so the
 * same scan decoded again for the next viewer was a new object and a miss).
 *
 * Entries are therefore keyed by the CONTENT that decides the mesh (a fingerprint of the certified points, normals, confidences, free-air points and
 * voxel sizes) plus the settings that can change the geometry, and the cache is shared by every viewer. It is bounded: a few entries and a byte budget,
 * least recently used first, so holding one more scan can never grow memory without limit (the post-scan memory budget is tight on phones).
 */
export interface TsdfFingerprintSource {
  tsdfPositions: Float32Array;
  tsdfNormals: Float32Array;
  tsdfCoverageConfidence: Float32Array;
  freeAirPositions: Float32Array;
  voxelSizeMeters: number;
  tsdfPointCount: number;
  occupancyVoxelSizeMeters?: number;
  occupancyFineVoxelSizeMeters?: number;
}

function hashFloats(state: [number, number], values: Float32Array): void {
  const words = new Uint32Array(values.buffer, values.byteOffset, values.length);
  let a = state[0];
  let b = state[1];
  for (let index = 0; index < words.length; index += 1) {
    const word = words[index];
    a = Math.imul(a ^ word, 0x01000193) >>> 0;                 /* FNV-1a */
    b = Math.imul(b ^ (word + index), 0x85ebca6b) >>> 0;       /* a second, independent mix: 64 bits in all */
  }
  state[0] = Math.imul(a ^ words.length, 0x01000193) >>> 0;
  state[1] = Math.imul(b ^ words.length, 0x85ebca6b) >>> 0;
}

/** Identity of the geometry the TSDF reconstruction is computed from. Same points and sizes -> same fingerprint, whichever object carries them. */
export function tsdfGeometryFingerprint(source: TsdfFingerprintSource): string {
  const state: [number, number] = [0x811c9dc5, 0x9e3779b9];
  hashFloats(state, source.tsdfPositions);
  hashFloats(state, source.tsdfNormals);
  hashFloats(state, source.tsdfCoverageConfidence);
  hashFloats(state, source.freeAirPositions);
  hashFloats(state, new Float32Array([source.voxelSizeMeters, source.tsdfPointCount, source.occupancyVoxelSizeMeters ?? 0, source.occupancyFineVoxelSizeMeters ?? 0]));
  return `${source.tsdfPointCount}:${state[0].toString(16)}${state[1].toString(16)}`;
}

interface Entry<Mesh> { key: string; mesh: Mesh; bytes: number }

export interface TsdfMeshCacheLimits { maxEntries: number; maxBytes: number }
export const DEFAULT_TSDF_CACHE_LIMITS: TsdfMeshCacheLimits = { maxEntries: 3, maxBytes: 48 * 1024 * 1024 };

export class TsdfMeshCache<Mesh> {
  private entries: Entry<Mesh>[] = [];        /* least recently used first */

  constructor(
    private readonly bytesOf: (mesh: Mesh) => number = () => 0,
    private readonly limits: TsdfMeshCacheLimits = DEFAULT_TSDF_CACHE_LIMITS,
  ) {}

  private static key(fingerprint: string, settingsKey: string): string { return `${fingerprint}|${settingsKey}`; }

  get(fingerprint: string, settingsKey: string): Mesh | null {
    const key = TsdfMeshCache.key(fingerprint, settingsKey);
    const index = this.entries.findIndex((entry) => entry.key === key);
    if (index < 0) return null;
    const [entry] = this.entries.splice(index, 1);
    this.entries.push(entry);                  /* most recently used */
    return entry.mesh;
  }

  set(fingerprint: string, settingsKey: string, mesh: Mesh): void {
    const key = TsdfMeshCache.key(fingerprint, settingsKey);
    this.entries = this.entries.filter((entry) => entry.key !== key);
    this.entries.push({ key, mesh, bytes: this.bytesOf(mesh) });
    this.evict();
  }

  /** Drops least recently used entries until the limits hold; a single entry larger than the byte budget is not kept at all. */
  private evict(): void {
    while (this.entries.length > this.limits.maxEntries) this.entries.shift();
    while (this.entries.length > 0 && this.totalBytes > this.limits.maxBytes) this.entries.shift();
  }

  get totalBytes(): number { return this.entries.reduce((sum, entry) => sum + entry.bytes, 0); }
  get size(): number { return this.entries.length; }

  /** Memory pressure: give everything back. */
  clear(): void { this.entries = []; }
}

/** The reconstruction settings that can change the geometry. Worker concurrency changes only how fast it is computed, not what is computed. */
export function tsdfSettingsKey(options: { allowOptionalAi: boolean }): string {
  return options.allowOptionalAi ? 'optional-ai' : 'baseline';
}
