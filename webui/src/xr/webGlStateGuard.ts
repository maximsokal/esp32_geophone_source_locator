export interface VertexAttribSnapshot {
  enabled: boolean;
  buffer: WebGLBuffer | null;
  size: number;
  type: number;
  normalized: boolean;
  stride: number;
  offset: number;
  divisor: number | null;
}

export interface WebGlStateSnapshot {
  framebuffer: WebGLFramebuffer | null;
  readFramebuffer: WebGLFramebuffer | null;
  drawFramebuffer: WebGLFramebuffer | null;
  viewport: Int32Array;
  program: WebGLProgram | null;
  arrayBuffer: WebGLBuffer | null;
  pixelPackBuffer: WebGLBuffer | null;
  activeTexture: number;
  activeTextureBinding2D: WebGLTexture | null;
  texture0Binding2D: WebGLTexture | null;
  vertexArray: WebGLVertexArrayObject | null;
  vertexAttrib: VertexAttribSnapshot | null;
}

type Gl = WebGLRenderingContext | WebGL2RenderingContext;

function isWebGl2(gl: Gl): gl is WebGL2RenderingContext {
  return typeof (gl as WebGL2RenderingContext).bindVertexArray === 'function' &&
    typeof (gl as WebGL2RenderingContext).READ_FRAMEBUFFER_BINDING === 'number';
}

function cloneViewport(value: unknown): Int32Array {
  if (value instanceof Int32Array && value.length >= 4) return new Int32Array(value.subarray(0, 4));
  const array = value as ArrayLike<number> | null;
  return new Int32Array([
    Number(array?.[0] ?? 0),
    Number(array?.[1] ?? 0),
    Number(array?.[2] ?? 1),
    Number(array?.[3] ?? 1),
  ]);
}

function captureAttrib(gl: WebGLRenderingContext, location: number): VertexAttribSnapshot | null {
  if (location < 0) return null;
  return {
    enabled: Boolean(gl.getVertexAttrib(location, gl.VERTEX_ATTRIB_ARRAY_ENABLED)),
    buffer: gl.getVertexAttrib(location, gl.VERTEX_ATTRIB_ARRAY_BUFFER_BINDING) as WebGLBuffer | null,
    size: Number(gl.getVertexAttrib(location, gl.VERTEX_ATTRIB_ARRAY_SIZE)),
    type: Number(gl.getVertexAttrib(location, gl.VERTEX_ATTRIB_ARRAY_TYPE)),
    normalized: Boolean(gl.getVertexAttrib(location, gl.VERTEX_ATTRIB_ARRAY_NORMALIZED)),
    stride: Number(gl.getVertexAttrib(location, gl.VERTEX_ATTRIB_ARRAY_STRIDE)),
    offset: Number(gl.getVertexAttribOffset(location, gl.VERTEX_ATTRIB_ARRAY_POINTER)),
    divisor: null,
  };
}

/** Capture only state modified by RawCameraGuidanceCapture. */
export function captureWebGlState(gl: Gl, vertexAttribLocation: number): WebGlStateSnapshot {
  const activeTexture = Number(gl.getParameter(gl.ACTIVE_TEXTURE));
  const activeTextureBinding2D = gl.getParameter(gl.TEXTURE_BINDING_2D) as WebGLTexture | null;
  gl.activeTexture(gl.TEXTURE0);
  const texture0Binding2D = gl.getParameter(gl.TEXTURE_BINDING_2D) as WebGLTexture | null;
  gl.activeTexture(activeTexture);

  const gl2 = isWebGl2(gl) ? gl : null;
  const vertexArray = gl2 === null
    ? null
    : gl2.getParameter(gl2.VERTEX_ARRAY_BINDING) as WebGLVertexArrayObject | null;
  const vertexAttrib = gl2 === null ? captureAttrib(gl, vertexAttribLocation) : null;

  return {
    framebuffer: gl.getParameter(gl.FRAMEBUFFER_BINDING) as WebGLFramebuffer | null,
    readFramebuffer: gl2 === null ? null : gl2.getParameter(gl2.READ_FRAMEBUFFER_BINDING) as WebGLFramebuffer | null,
    drawFramebuffer: gl2 === null ? null : gl2.getParameter(gl2.DRAW_FRAMEBUFFER_BINDING) as WebGLFramebuffer | null,
    viewport: cloneViewport(gl.getParameter(gl.VIEWPORT)),
    program: gl.getParameter(gl.CURRENT_PROGRAM) as WebGLProgram | null,
    arrayBuffer: gl.getParameter(gl.ARRAY_BUFFER_BINDING) as WebGLBuffer | null,
    pixelPackBuffer: gl2 === null
      ? null
      : gl2.getParameter(gl2.PIXEL_PACK_BUFFER_BINDING) as WebGLBuffer | null,
    activeTexture,
    activeTextureBinding2D,
    texture0Binding2D,
    vertexArray,
    vertexAttrib,
  };
}

export interface WebGlRasterStateSnapshot {
  blendEnabled: boolean;
  depthTestEnabled: boolean;
  depthMask: boolean;
  scissorEnabled: boolean;
  cullFaceEnabled: boolean;
  stencilTestEnabled: boolean;
  colorMask: readonly [boolean, boolean, boolean, boolean];
}

/** Capture the raster bits changed by the camera-copy pass. */
export function captureWebGlRasterState(gl: Gl): WebGlRasterStateSnapshot {
  const mask = gl.getParameter(gl.COLOR_WRITEMASK) as ArrayLike<boolean> | null;
  return {
    blendEnabled: gl.isEnabled(gl.BLEND),
    depthTestEnabled: gl.isEnabled(gl.DEPTH_TEST),
    depthMask: Boolean(gl.getParameter(gl.DEPTH_WRITEMASK)),
    scissorEnabled: gl.isEnabled(gl.SCISSOR_TEST),
    cullFaceEnabled: gl.isEnabled(gl.CULL_FACE),
    stencilTestEnabled: gl.isEnabled(gl.STENCIL_TEST),
    colorMask: [Boolean(mask?.[0] ?? true), Boolean(mask?.[1] ?? true), Boolean(mask?.[2] ?? true), Boolean(mask?.[3] ?? true)],
  };
}

export function restoreWebGlRasterState(gl: Gl, snapshot: WebGlRasterStateSnapshot): void {
  if (snapshot.blendEnabled) gl.enable(gl.BLEND); else gl.disable(gl.BLEND);
  if (snapshot.depthTestEnabled) gl.enable(gl.DEPTH_TEST); else gl.disable(gl.DEPTH_TEST);
  gl.depthMask(snapshot.depthMask);
  if (snapshot.scissorEnabled) gl.enable(gl.SCISSOR_TEST); else gl.disable(gl.SCISSOR_TEST);
  if (snapshot.cullFaceEnabled) gl.enable(gl.CULL_FACE); else gl.disable(gl.CULL_FACE);
  if (snapshot.stencilTestEnabled) gl.enable(gl.STENCIL_TEST); else gl.disable(gl.STENCIL_TEST);
  gl.colorMask(snapshot.colorMask[0], snapshot.colorMask[1], snapshot.colorMask[2], snapshot.colorMask[3]);
}

/** Establish the raster state needed for an exact camera-texture copy. */
export function setWebGlCopyRasterState(gl: Gl): void {
  gl.disable(gl.BLEND);
  gl.disable(gl.DEPTH_TEST);
  gl.disable(gl.SCISSOR_TEST);
  gl.disable(gl.STENCIL_TEST);
  gl.disable(gl.CULL_FACE);
  gl.colorMask(true, true, true, true);
  gl.depthMask(true);
}

export interface WebGlDrawStateSnapshot {
  state: WebGlStateSnapshot;
  raster: WebGlRasterStateSnapshot;
  blendSrcRgb: number;
  blendDstRgb: number;
  blendSrcAlpha: number;
  blendDstAlpha: number;
  blendEquationRgb: number;
  blendEquationAlpha: number;
  depthFunc: number;
  attributes: readonly { location: number; snapshot: VertexAttribSnapshot | null }[];
}

/** Capture vertex attributes as well as the shared draw state the coverage renderer mutates. */
export function captureWebGlDrawState(gl: Gl, locations: readonly number[]): WebGlDrawStateSnapshot {
  return {
    state: captureWebGlState(gl, -1),
    raster: captureWebGlRasterState(gl),
    blendSrcRgb: Number(gl.getParameter(gl.BLEND_SRC_RGB)),
    blendDstRgb: Number(gl.getParameter(gl.BLEND_DST_RGB)),
    blendSrcAlpha: Number(gl.getParameter(gl.BLEND_SRC_ALPHA)),
    blendDstAlpha: Number(gl.getParameter(gl.BLEND_DST_ALPHA)),
    blendEquationRgb: Number(gl.getParameter(gl.BLEND_EQUATION_RGB)),
    blendEquationAlpha: Number(gl.getParameter(gl.BLEND_EQUATION_ALPHA)),
    depthFunc: Number(gl.getParameter(gl.DEPTH_FUNC)),
    attributes: locations.map((location) => ({ location, snapshot: captureAttrib(gl, location) })),
  };
}

/** Restore the prior VAO before replaying its attribute pointers and enable bits. */
export function restoreWebGlDrawState(gl: Gl, snapshot: WebGlDrawStateSnapshot): void {
  const gl2 = isWebGl2(gl) ? gl : null;
  if (gl2 !== null) gl2.bindVertexArray(snapshot.state.vertexArray);
  for (const attribute of snapshot.attributes) restoreAttrib(gl, attribute.location, attribute.snapshot);
  restoreWebGlState(gl, -1, snapshot.state);
  restoreWebGlRasterState(gl, snapshot.raster);
  gl.blendFuncSeparate(snapshot.blendSrcRgb, snapshot.blendDstRgb, snapshot.blendSrcAlpha, snapshot.blendDstAlpha);
  gl.blendEquationSeparate(snapshot.blendEquationRgb, snapshot.blendEquationAlpha);
  gl.depthFunc(snapshot.depthFunc);
}

function restoreAttrib(
  gl: WebGLRenderingContext,
  location: number,
  snapshot: VertexAttribSnapshot | null,
): void {
  if (location < 0 || snapshot === null) return;
  if (snapshot.buffer !== null) {
    gl.bindBuffer(gl.ARRAY_BUFFER, snapshot.buffer);
    gl.vertexAttribPointer(
      location,
      snapshot.size,
      snapshot.type,
      snapshot.normalized,
      snapshot.stride,
      snapshot.offset,
    );
  }
  if (snapshot.enabled) gl.enableVertexAttribArray(location);
  else gl.disableVertexAttribArray(location);
}

/** Restore all state captured above, including non-zero active texture units. */
export function restoreWebGlState(gl: Gl, vertexAttribLocation: number, snapshot: WebGlStateSnapshot): void {
  const gl2 = isWebGl2(gl) ? gl : null;

  gl.activeTexture(gl.TEXTURE0);
  gl.bindTexture(gl.TEXTURE_2D, snapshot.texture0Binding2D);
  if (snapshot.activeTexture !== gl.TEXTURE0) {
    gl.activeTexture(snapshot.activeTexture);
    gl.bindTexture(gl.TEXTURE_2D, snapshot.activeTextureBinding2D);
  } else {
    gl.activeTexture(snapshot.activeTexture);
  }

  if (gl2 !== null) {
    gl2.bindVertexArray(snapshot.vertexArray);
    gl2.bindFramebuffer(gl2.READ_FRAMEBUFFER, snapshot.readFramebuffer);
    gl2.bindFramebuffer(gl2.DRAW_FRAMEBUFFER, snapshot.drawFramebuffer);
    gl2.bindBuffer(gl2.PIXEL_PACK_BUFFER, snapshot.pixelPackBuffer);
  } else {
    restoreAttrib(gl, vertexAttribLocation, snapshot.vertexAttrib);
    gl.bindFramebuffer(gl.FRAMEBUFFER, snapshot.framebuffer);
  }
  gl.bindBuffer(gl.ARRAY_BUFFER, snapshot.arrayBuffer);
  gl.useProgram(snapshot.program);
  gl.viewport(snapshot.viewport[0], snapshot.viewport[1], snapshot.viewport[2], snapshot.viewport[3]);
}

export function webGl2Context(gl: Gl): WebGL2RenderingContext | null {
  return isWebGl2(gl) ? gl : null;
}
