export interface PixelReadbackResult<T> {
  pixels: Uint8Array;
  metadata: T;
}

interface ReadbackSlot<T> {
  buffer: WebGLBuffer;
  sync: WebGLSync | null;
  metadata: T | null;
  sequence: number;
}

/**
 * Small WebGL2 PIXEL_PACK_BUFFER ring for camera guidance readback.
 *
 * The XR render callback must never wait for the GPU. enqueue() writes pixels
 * GPU->GPU into a PBO and attaches a fence. poll() uses clientWaitSync(..., 0)
 * and calls getBufferSubData only after the oldest fence has already signalled.
 * If every slot is still in flight, the new advisory RGB frame is dropped.
 * Physical XR depth/fusion is therefore never delayed by camera guidance.
 *
 * The caller owns surrounding WebGL state restoration; RawCameraGuidanceCapture
 * wraps enqueue/poll in webGlStateGuard so PIXEL_PACK_BUFFER state is preserved.
 */
export class NonBlockingRgbaReadback<T> {
  private readonly slots: ReadbackSlot<T>[] = [];
  private readonly output: Uint8Array;
  private nextSequence = 1;
  private disposed = false;

  constructor(
    private readonly gl: WebGL2RenderingContext,
    readonly width: number,
    readonly height: number,
    slotCount = 3,
  ) {
    const byteLength = Math.max(1, Math.floor(width)) * Math.max(1, Math.floor(height)) * 4;
    this.output = new Uint8Array(byteLength);
    const count = Math.max(2, Math.min(4, Math.floor(slotCount)));
    const previous = gl.getParameter(gl.PIXEL_PACK_BUFFER_BINDING) as WebGLBuffer | null;
    try {
      for (let index = 0; index < count; index += 1) {
        const buffer = gl.createBuffer();
        if (buffer === null) throw new Error('Unable to allocate camera pixel-pack buffer.');
        gl.bindBuffer(gl.PIXEL_PACK_BUFFER, buffer);
        gl.bufferData(gl.PIXEL_PACK_BUFFER, byteLength, gl.STREAM_READ);
        this.slots.push({ buffer, sync: null, metadata: null, sequence: 0 });
      }
    } catch (error) {
      for (const slot of this.slots) gl.deleteBuffer(slot.buffer);
      this.slots.length = 0;
      throw error;
    } finally {
      gl.bindBuffer(gl.PIXEL_PACK_BUFFER, previous);
    }
  }

  get pendingCount(): number {
    let count = 0;
    for (const slot of this.slots) if (slot.sync !== null) count += 1;
    return count;
  }

  /** Queue the currently bound READ framebuffer without a CPU readback. */
  enqueue(metadata: T): boolean {
    if (this.disposed) return false;
    const slot = this.slots.find((candidate) => candidate.sync === null);
    if (slot === undefined) return false;

    const gl = this.gl;
    gl.bindBuffer(gl.PIXEL_PACK_BUFFER, slot.buffer);
    gl.readPixels(0, 0, this.width, this.height, gl.RGBA, gl.UNSIGNED_BYTE, 0);
    const sync = gl.fenceSync(gl.SYNC_GPU_COMMANDS_COMPLETE, 0);
    gl.bindBuffer(gl.PIXEL_PACK_BUFFER, null);
    if (sync === null) return false;

    slot.sync = sync;
    slot.metadata = metadata;
    slot.sequence = this.nextSequence++;
    return true;
  }

  /**
   * Return the oldest completed frame, or null immediately when it is not ready.
   * GPU commands execute in order, so if the oldest fence has not signalled yet,
   * checking newer slots cannot produce a useful earlier frame.
   */
  poll(): PixelReadbackResult<T> | null {
    if (this.disposed) return null;
    let oldest: ReadbackSlot<T> | null = null;
    for (const slot of this.slots) {
      if (slot.sync === null) continue;
      if (oldest === null || slot.sequence < oldest.sequence) oldest = slot;
    }
    if (oldest === null || oldest.sync === null || oldest.metadata === null) return null;

    const gl = this.gl;
    const status = gl.clientWaitSync(oldest.sync, 0, 0);
    if (status === gl.TIMEOUT_EXPIRED) return null;
    if (status === gl.WAIT_FAILED) {
      gl.deleteSync(oldest.sync);
      oldest.sync = null;
      oldest.metadata = null;
      oldest.sequence = 0;
      return null;
    }
    if (status !== gl.ALREADY_SIGNALED && status !== gl.CONDITION_SATISFIED) return null;

    const metadata = oldest.metadata;
    gl.bindBuffer(gl.PIXEL_PACK_BUFFER, oldest.buffer);
    gl.getBufferSubData(gl.PIXEL_PACK_BUFFER, 0, this.output);
    gl.bindBuffer(gl.PIXEL_PACK_BUFFER, null);
    gl.deleteSync(oldest.sync);
    oldest.sync = null;
    oldest.metadata = null;
    oldest.sequence = 0;
    return { pixels: this.output, metadata };
  }

  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    for (const slot of this.slots) {
      if (slot.sync !== null) this.gl.deleteSync(slot.sync);
      this.gl.deleteBuffer(slot.buffer);
      slot.sync = null;
      slot.metadata = null;
      slot.sequence = 0;
    }
    this.slots.length = 0;
  }
}
