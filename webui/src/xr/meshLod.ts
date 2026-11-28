import { clientDiagnostics } from '../data/clientDiagnostics';
import { indexTriangleSoup } from './meshIndex';

/**
 * The simplifier's error limit, in voxels. It is a quadric estimate, not a hard bound on the distance: on the
 * 2026-11-26 room scan at 0.05 (2.8 mm) 72% of the triangles go, 99% of the surface stays within 2.9 mm and all of it
 * within 6.1 mm both ways, with the borders, components and defect counts unchanged (beside a lone 3 cm spike a test
 * saw 10 mm). At 0.1 and above small features are lost (up to 49 mm) and faces start to cross.
 */
export const DISPLAY_LOD_TOLERANCE_VOXELS = 0.05;

export type SimplifierModule = Pick<typeof import('meshoptimizer/simplifier'), 'MeshoptSimplifier'>;

export interface DisplayLod {
  trianglePositions: Float32Array;
  /** false: the input is returned as it was */
  simplified: boolean;
}

/** Vertices of edges used by more than two faces: they are kept where they are. */
function lockedVertices(indices: Uint32Array, vertexCount: number): Uint8Array {
  const uses = new Map<number, number>();
  const forEachEdge = (visit: (key: number, a: number, b: number) => void): void => {
    for (let corner = 0; corner < indices.length; corner += 3) {
      for (let side = 0; side < 3; side += 1) {
        const a = indices[corner + side], b = indices[corner + (side + 1) % 3];
        visit(a < b ? a * vertexCount + b : b * vertexCount + a, a, b);
      }
    }
  };
  forEachEdge((key) => { uses.set(key, (uses.get(key) ?? 0) + 1); });
  const locked = new Uint8Array(vertexCount);
  forEachEdge((key, a, b) => { if (uses.get(key)! > 2) { locked[a] = 1; locked[b] = 1; } });
  return locked;
}

/**
 * A lighter copy of a reconstructed mesh for drawing only; the reconstruction, its diagnostics and the physical data
 * stay as they are. meshoptimizer collapses edges within the tolerance. Border edges (doorways, windows, the limits
 * of the observed surface) and vertices of ambiguous edges are locked, and separate shells (a thin wall) share no edge,
 * so they cannot be merged. Where the simplifier cannot run, the input comes back unchanged.
 */
export async function simplifyMeshForDisplay(
  trianglePositions: Float32Array,
  voxelSizeMeters: number,
  load: () => Promise<SimplifierModule> = () => import('meshoptimizer/simplifier'),
): Promise<DisplayLod> {
  const unchanged: DisplayLod = { trianglePositions, simplified: false };
  const tolerance = Math.abs(voxelSizeMeters) * DISPLAY_LOD_TOLERANCE_VOXELS;
  if (trianglePositions.length < 18 || !(tolerance > 0)) return unchanged;
  try {
    const { MeshoptSimplifier } = await load();
    if (!MeshoptSimplifier.supported) throw new Error('WebAssembly simplifier is not supported here');
    await MeshoptSimplifier.ready;
    const mesh = indexTriangleSoup(trianglePositions);
    const indices = Uint32Array.from(mesh.indices);
    const vertexCount = mesh.positions.length / 3;
    const [kept] = MeshoptSimplifier.simplifyWithAttributes(
      indices, mesh.positions, 3, new Float32Array(0), 0, [], lockedVertices(indices, vertexCount),
      0, tolerance, ['LockBorder', 'ErrorAbsolute'],
    );
    if (kept.length === 0 || kept.length >= indices.length) return unchanged;
    const soup = new Float32Array(kept.length * 3);
    for (let corner = 0; corner < kept.length; corner += 1) {
      const vertex = kept[corner] * 3;
      soup[corner * 3] = mesh.positions[vertex]; soup[corner * 3 + 1] = mesh.positions[vertex + 1]; soup[corner * 3 + 2] = mesh.positions[vertex + 2];
    }
    return { trianglePositions: soup, simplified: true };
  } catch (error) {
    clientDiagnostics.warn('SCAN_MESH_LOD_UNAVAILABLE', error);
    return unchanged;
  }
}
