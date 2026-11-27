/**
 * Sparse voxel block grid of the post-scan TSDF, in the architecture of Open3D's VoxelBlockGrid
 * (https://www.open3d.org/docs/latest/tutorial/t_reconstruction_system/voxel_block_grid.html): a hash map from BLOCK coordinates to dense blocks of
 * 8 x 8 x 8 voxels held in typed arrays. Blocks are allocated only where a voxel is written.
 *
 * Two properties the extraction depends on are kept from the per-voxel map it replaces: voxels are remembered in the ORDER THEY WERE CREATED (the triangles
 * come out in that order, so the mesh is bit for bit the same), and the number of voxels (not of blocks) is capped. Sums are 64-bit floats, as before.
 */
export const TSDF_BLOCK_SHIFT = 3;
export const TSDF_BLOCK_SIDE = 1 << TSDF_BLOCK_SHIFT;
export const TSDF_BLOCK_VOXELS = TSDF_BLOCK_SIDE * TSDF_BLOCK_SIDE * TSDF_BLOCK_SIDE;
const LOCAL = TSDF_BLOCK_SIDE - 1;

/** One number per block coordinate: 17 bits per axis around the origin. */
function blockKey(x: number, y: number, z: number): number {
  return ((x + 65536) * 131072 + (y + 65536)) * 131072 + (z + 65536);
}

export interface TsdfBlock {
  readonly x: number;
  readonly y: number;
  readonly z: number;
  /** Position of the block in {@link TsdfBlockGrid.blocks}. */
  readonly index: number;
  /** Sum of weight x truncated signed distance, per voxel; valid where `weight` is above zero. */
  readonly weightedValue: Float64Array;
  /** Sum of the observation weights, per voxel; zero means the voxel does not exist. */
  readonly weight: Float64Array;
}

export class TsdfBlockGrid {
  readonly blocks: TsdfBlock[] = [];
  private readonly byKey = new Map<number, TsdfBlock>();
  private last: TsdfBlock | null = null;
  /** Existing voxels in creation order, each as block index x 512 + position inside the block. */
  private order = new Int32Array(4096);
  private count = 0;

  constructor(readonly maxVoxels: number) {}

  get voxelCount(): number { return this.count; }

  /** The voxel created `n`-th, as block index x 512 + position inside the block. */
  voxelAt(n: number): number { return this.order[n]; }

  /** The block at these BLOCK coordinates, if any voxel was written there. */
  block(x: number, y: number, z: number): TsdfBlock | undefined {
    const last = this.last;
    if (last !== null && last.x === x && last.y === y && last.z === z) return last;
    const found = this.byKey.get(blockKey(x, y, z));
    if (found !== undefined) this.last = found;
    return found;
  }

  /** Adds one observation (weight above zero) to the voxel at these VOXEL coordinates; a new voxel beyond the cap is dropped. */
  add(ix: number, iy: number, iz: number, value: number, observationWeight: number): void {
    const bx = ix >> TSDF_BLOCK_SHIFT; const by = iy >> TSDF_BLOCK_SHIFT; const bz = iz >> TSDF_BLOCK_SHIFT;
    const local = (((ix & LOCAL) << TSDF_BLOCK_SHIFT | (iy & LOCAL)) << TSDF_BLOCK_SHIFT) | (iz & LOCAL);
    let block = this.block(bx, by, bz);
    if (block !== undefined && block.weight[local] > 0) {
      block.weightedValue[local] += value * observationWeight;
      block.weight[local] += observationWeight;
      return;
    }
    if (this.count >= this.maxVoxels) return;
    if (block === undefined) {
      block = { x: bx, y: by, z: bz, index: this.blocks.length, weightedValue: new Float64Array(TSDF_BLOCK_VOXELS), weight: new Float64Array(TSDF_BLOCK_VOXELS) };
      this.blocks.push(block);
      this.byKey.set(blockKey(bx, by, bz), block);
      this.last = block;
    }
    block.weightedValue[local] = value * observationWeight;
    block.weight[local] = observationWeight;
    if (this.count === this.order.length) { const grown = new Int32Array(this.order.length * 2); grown.set(this.order); this.order = grown; }
    this.order[this.count] = block.index * TSDF_BLOCK_VOXELS + local;
    this.count += 1;
  }
}
