/**
 * A number per distinct corner position of a soup, compared bit for bit (open addressing over the 32-bit floats): the stitching emits a shared vertex with
 * identical coordinates in every triangle that uses it. Formatting the coordinates into strings cost more than the check itself.
 * The number is the first corner that has the position.
 */
export function soupVertexIds(soup: Float32Array): Int32Array {
  const corners = Math.floor(soup.length / 3);
  const bits = new Uint32Array(soup.buffer, soup.byteOffset, corners * 3);
  let size = 16; while (size < corners * 2) size *= 2;
  const table = new Int32Array(size).fill(-1);
  const ids = new Int32Array(corners);
  for (let corner = 0; corner < corners; corner += 1) {
    const x = bits[corner * 3]; const y = bits[corner * 3 + 1]; const z = bits[corner * 3 + 2];
    let slot = (Math.imul(x, 73856093) ^ Math.imul(y, 19349663) ^ Math.imul(z, 83492791)) >>> 0;
    slot = (slot ^ (slot >>> 15)) & (size - 1);
    for (;;) {
      const first = table[slot];
      if (first < 0) { table[slot] = corner; ids[corner] = corner; break; }
      if (bits[first * 3] === x && bits[first * 3 + 1] === y && bits[first * 3 + 2] === z) { ids[corner] = first; break; }
      slot = (slot + 1) & (size - 1);
    }
  }
  return ids;
}

export interface IndexedMesh {
  positions: Float32Array;
  /** 16 bit while every vertex number fits (0xffff is left free: WebGL2 reads it as a primitive restart) */
  indices: Uint16Array | Uint32Array;
}

/** Lossless: expanding the result gives the soup back bit for bit. Only whole triangles are taken. */
export function indexTriangleSoup(soup: Float32Array): IndexedMesh {
  const corners = Math.floor(soup.length / 9) * 3;
  const ids = soupVertexIds(soup);
  const number = new Int32Array(corners);
  let vertexCount = 0;
  for (let corner = 0; corner < corners; corner += 1) {
    if (ids[corner] === corner) { number[corner] = vertexCount; vertexCount += 1; }
  }
  const positions = new Float32Array(vertexCount * 3);
  const indices = vertexCount <= 0xffff ? new Uint16Array(corners) : new Uint32Array(corners);
  const from = new Uint32Array(soup.buffer, soup.byteOffset, corners * 3);
  const to = new Uint32Array(positions.buffer);
  for (let corner = 0; corner < corners; corner += 1) {
    const first = ids[corner];
    const vertex = number[first];
    indices[corner] = vertex;
    if (first !== corner) continue;
    to[vertex * 3] = from[corner * 3]; to[vertex * 3 + 1] = from[corner * 3 + 1]; to[vertex * 3 + 2] = from[corner * 3 + 2];
  }
  return { positions, indices };
}
