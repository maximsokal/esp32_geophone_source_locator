/** Identity of a welded vertex. The reconstruction uses small integer ids; hand-built meshes may use strings. */
export type MeshVertexKey = string | number;

const VERTEX_ID_LIMIT = 2 ** 26;
const PACKED_AXIS_LIMIT = 65536;

/** One key for an undirected edge. Two ids below 2^26 pack into an exact number, with no string to build and hash. */
export function meshEdgeKey(a: MeshVertexKey, b: MeshVertexKey): MeshVertexKey {
  if (typeof a === 'number' && typeof b === 'number') return a < b ? a * VERTEX_ID_LIMIT + b : b * VERTEX_ID_LIMIT + a;
  return a < b ? `${a}|${b}` : `${b}|${a}`;
}

/** Welds points that round to the same quantization cell: equal cells give equal ids, in order of first appearance. */
export function createMeshVertexIds(quantizationMeters: number): (x: number, y: number, z: number) => number {
  // Cells near the origin pack into one exact number; a cell further out keeps all its digits in a string.
  const ids = new Map<number | string, number>();
  return (x, y, z) => {
    const qx = Math.round(x / quantizationMeters);
    const qy = Math.round(y / quantizationMeters);
    const qz = Math.round(z / quantizationMeters);
    const key = Math.abs(qx) < PACKED_AXIS_LIMIT && Math.abs(qy) < PACKED_AXIS_LIMIT && Math.abs(qz) < PACKED_AXIS_LIMIT
      ? ((qx + PACKED_AXIS_LIMIT) * 131072 + (qy + PACKED_AXIS_LIMIT)) * 131072 + (qz + PACKED_AXIS_LIMIT)
      : `${qx}:${qy}:${qz}`;
    let id = ids.get(key);
    if (id === undefined) {
      id = ids.size;
      ids.set(key, id);
    }
    return id;
  };
}
