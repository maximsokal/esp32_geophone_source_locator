import {
  buildRgbDepthGuidanceFrame,
  type RgbDepthGuidanceFrame,
} from './rgbDepthGuidance';
import {
  captureWebGlState,
  restoreWebGlState,
  webGl2Context,
} from './webGlStateGuard';

export interface XrCameraLike {
  width: number;
  height: number;
}

export interface XrCameraViewLike {
  camera?: XrCameraLike | null;
  projectionMatrix?: ArrayLike<number>;
  transform?: { matrix: ArrayLike<number> };
}

export interface XrWebGlBindingLike {
  getCameraImage(camera: XrCameraLike): WebGLTexture | null;
}

export type XrWebGlBindingConstructorLike = new (
  session: object,
  context: WebGLRenderingContext | WebGL2RenderingContext,
) => XrWebGlBindingLike;

const TARGET_WIDTH = 256;
const TARGET_HEIGHT = 144;

function compileShader(
  gl: WebGLRenderingContext | WebGL2RenderingContext,
  type: number,
  source: string,
): WebGLShader {
  const shader = gl.createShader(type);
  if (shader === null) throw new Error('Unable to allocate raw-camera guidance shader.');
  gl.shaderSource(shader, source);
  gl.compileShader(shader);
  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    const message = gl.getShaderInfoLog(shader) ?? 'shader compilation failed';
    gl.deleteShader(shader);
    throw new Error(`Raw-camera guidance shader failed: ${message}`);
  }
  return shader;
}

function createProgram(gl: WebGLRenderingContext | WebGL2RenderingContext): WebGLProgram {
  // GLSL ES 1.00 is used because the runtime may fall back to WebGL1. Chromium
  // exposes the XR camera as a normal bottom-left-origin opaque WebGL texture.
  // We only sample it; ownership and lifetime remain with the user agent.
  const vertex = compileShader(gl, gl.VERTEX_SHADER, `
    attribute vec2 a_position;
    varying vec2 v_uv;
    void main() {
      v_uv = a_position * 0.5 + 0.5;
      gl_Position = vec4(a_position, 0.0, 1.0);
    }
  `);
  const fragment = compileShader(gl, gl.FRAGMENT_SHADER, `
    precision mediump float;
    uniform sampler2D u_camera;
    varying vec2 v_uv;
    void main() {
      gl_FragColor = texture2D(u_camera, v_uv);
    }
  `);
  const program = gl.createProgram();
  if (program === null) {
    gl.deleteShader(vertex);
    gl.deleteShader(fragment);
    throw new Error('Unable to allocate raw-camera guidance program.');
  }
  gl.attachShader(program, vertex);
  gl.attachShader(program, fragment);
  gl.linkProgram(program);
  gl.deleteShader(vertex);
  gl.deleteShader(fragment);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
    const message = gl.getProgramInfoLog(program) ?? 'program link failed';
    gl.deleteProgram(program);
    throw new Error(`Raw-camera guidance program failed: ${message}`);
  }
  return program;
}

export class RawCameraGuidanceCapture {
  private readonly program: WebGLProgram;
  private readonly framebuffer: WebGLFramebuffer;
  private readonly targetTexture: WebGLTexture;
  private readonly vertexBuffer: WebGLBuffer;
  private readonly positionLocation: number;
  private readonly samplerLocation: WebGLUniformLocation;
  private readonly vertexArray: WebGLVertexArrayObject | null;
  private readonly pixels = new Uint8Array(TARGET_WIDTH * TARGET_HEIGHT * 4);
  private disposed = false;

  constructor(private readonly gl: WebGLRenderingContext | WebGL2RenderingContext) {
    const framebuffer = gl.createFramebuffer();
    const texture = gl.createTexture();
    const buffer = gl.createBuffer();
    if (framebuffer === null || texture === null || buffer === null) {
      if (framebuffer !== null) gl.deleteFramebuffer(framebuffer);
      if (texture !== null) gl.deleteTexture(texture);
      if (buffer !== null) gl.deleteBuffer(buffer);
      throw new Error('Unable to allocate raw-camera guidance GPU resources.');
    }

    const program = createProgram(gl);
    const positionLocation = gl.getAttribLocation(program, 'a_position');
    const samplerLocation = gl.getUniformLocation(program, 'u_camera');
    if (positionLocation < 0 || samplerLocation === null) {
      gl.deleteProgram(program);
      gl.deleteFramebuffer(framebuffer);
      gl.deleteTexture(texture);
      gl.deleteBuffer(buffer);
      throw new Error('Raw-camera guidance shader inputs are unavailable.');
    }

    const gl2 = webGl2Context(gl);
    const vertexArray = gl2?.createVertexArray() ?? null;
    // Falling back to mutating the caller's WebGL2 VAO would defeat the state
    // isolation guarantee. WebGL1 has no core VAO and is restored explicitly.
    if (gl2 !== null && vertexArray === null) {
      gl.deleteProgram(program);
      gl.deleteFramebuffer(framebuffer);
      gl.deleteTexture(texture);
      gl.deleteBuffer(buffer);
      throw new Error('Unable to allocate isolated raw-camera WebGL2 vertex array.');
    }

    this.program = program;
    this.framebuffer = framebuffer;
    this.targetTexture = texture;
    this.vertexBuffer = buffer;
    this.positionLocation = positionLocation;
    this.samplerLocation = samplerLocation;
    this.vertexArray = vertexArray;
    const previous = captureWebGlState(gl, positionLocation);

    try {
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, texture);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      gl.texImage2D(
        gl.TEXTURE_2D, 0, gl.RGBA, TARGET_WIDTH, TARGET_HEIGHT, 0,
        gl.RGBA, gl.UNSIGNED_BYTE, null,
      );
      gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer);
      gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, texture, 0);
      if (gl.checkFramebufferStatus(gl.FRAMEBUFFER) !== gl.FRAMEBUFFER_COMPLETE) {
        throw new Error('Raw-camera guidance framebuffer is incomplete.');
      }

      gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
      gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([
        -1, -1, 1, -1, -1, 1,
        -1, 1, 1, -1, 1, 1,
      ]), gl.STATIC_DRAW);

      // A private VAO avoids touching the XR renderer's vertex-attrib state on
      // WebGL2. WebGL1 is protected by the state guard on each capture call.
      if (gl2 !== null && vertexArray !== null) {
        gl2.bindVertexArray(vertexArray);
        gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
        gl.enableVertexAttribArray(positionLocation);
        gl.vertexAttribPointer(positionLocation, 2, gl.FLOAT, false, 0, 0);
      }
    } catch (error) {
      if (gl2 !== null && vertexArray !== null) gl2.deleteVertexArray(vertexArray);
      gl.deleteProgram(program);
      gl.deleteFramebuffer(framebuffer);
      gl.deleteTexture(texture);
      gl.deleteBuffer(buffer);
      throw error;
    } finally {
      restoreWebGlState(gl, positionLocation, previous);
    }
  }

  capture(
    binding: XrWebGlBindingLike,
    view: XrCameraViewLike,
    capturedAtMs: number,
  ): RgbDepthGuidanceFrame | null {
    if (this.disposed || view.camera == null) return null;
    const cameraTexture = binding.getCameraImage(view.camera);
    if (cameraTexture === null) return null;

    const gl = this.gl;
    const gl2 = webGl2Context(gl);
    const previous = captureWebGlState(gl, this.positionLocation);

    try {
      gl.bindFramebuffer(gl.FRAMEBUFFER, this.framebuffer);
      gl.viewport(0, 0, TARGET_WIDTH, TARGET_HEIGHT);
      gl.useProgram(this.program);
      if (gl2 !== null && this.vertexArray !== null) {
        gl2.bindVertexArray(this.vertexArray);
      } else {
        gl.bindBuffer(gl.ARRAY_BUFFER, this.vertexBuffer);
        gl.enableVertexAttribArray(this.positionLocation);
        gl.vertexAttribPointer(this.positionLocation, 2, gl.FLOAT, false, 0, 0);
      }
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, cameraTexture);
      gl.uniform1i(this.samplerLocation, 0);
      gl.drawArrays(gl.TRIANGLES, 0, 6);
      gl.readPixels(0, 0, TARGET_WIDTH, TARGET_HEIGHT, gl.RGBA, gl.UNSIGNED_BYTE, this.pixels);
      const guidance = buildRgbDepthGuidanceFrame(
        this.pixels,
        TARGET_WIDTH,
        TARGET_HEIGHT,
        TARGET_WIDTH,
        TARGET_HEIGHT,
        'bottom-left',
        capturedAtMs,
      );
      if (
        view.projectionMatrix !== undefined && view.projectionMatrix.length >= 16 &&
        view.transform?.matrix !== undefined && view.transform.matrix.length >= 16
      ) {
        return {
          ...guidance,
          projectionMatrix: Float32Array.from(view.projectionMatrix),
          worldFromView: Float32Array.from(view.transform.matrix),
        };
      }
      return guidance;
    } finally {
      restoreWebGlState(gl, this.positionLocation, previous);
    }
  }

  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    const gl2 = webGl2Context(this.gl);
    if (gl2 !== null && this.vertexArray !== null) gl2.deleteVertexArray(this.vertexArray);
    this.gl.deleteProgram(this.program);
    this.gl.deleteFramebuffer(this.framebuffer);
    this.gl.deleteTexture(this.targetTexture);
    this.gl.deleteBuffer(this.vertexBuffer);
  }
}

export const RAW_CAMERA_GUIDANCE_WIDTH = TARGET_WIDTH;
export const RAW_CAMERA_GUIDANCE_HEIGHT = TARGET_HEIGHT;
