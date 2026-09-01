export type ProjectPoint = readonly [number, number, number];

export interface NormalizedProjection {
  u: number;
  v: number;
  clipDepth: number;
  /** Positive metric distance along the camera forward axis. */
  viewDepthMeters: number;
  visible: boolean;
}

function finite16(matrix: ArrayLike<number>): boolean {
  if (matrix.length < 16) return false;
  for (let index = 0; index < 16; index += 1) if (!Number.isFinite(Number(matrix[index]))) return false;
  return true;
}

export function rigidInverse(matrix: ArrayLike<number>, out = new Float32Array(16)): Float32Array | null {
  if (!finite16(matrix)) return null;
  const m0 = Number(matrix[0]); const m1 = Number(matrix[1]); const m2 = Number(matrix[2]);
  const m4 = Number(matrix[4]); const m5 = Number(matrix[5]); const m6 = Number(matrix[6]);
  const m8 = Number(matrix[8]); const m9 = Number(matrix[9]); const m10 = Number(matrix[10]);
  const tx = Number(matrix[12]); const ty = Number(matrix[13]); const tz = Number(matrix[14]);
  out[0]=m0; out[1]=m4; out[2]=m8; out[3]=0;
  out[4]=m1; out[5]=m5; out[6]=m9; out[7]=0;
  out[8]=m2; out[9]=m6; out[10]=m10; out[11]=0;
  out[12]=-(m0*tx+m1*ty+m2*tz);
  out[13]=-(m4*tx+m5*ty+m6*tz);
  out[14]=-(m8*tx+m9*ty+m10*tz);
  out[15]=1;
  return out;
}

export function multiplyMat4(left: ArrayLike<number>, right: ArrayLike<number>, out = new Float32Array(16)): Float32Array | null {
  if (!finite16(left) || !finite16(right)) return null;
  for (let column=0; column<4; column+=1) {
    const r0=Number(right[column*4]); const r1=Number(right[column*4+1]);
    const r2=Number(right[column*4+2]); const r3=Number(right[column*4+3]);
    out[column*4]=Number(left[0])*r0+Number(left[4])*r1+Number(left[8])*r2+Number(left[12])*r3;
    out[column*4+1]=Number(left[1])*r0+Number(left[5])*r1+Number(left[9])*r2+Number(left[13])*r3;
    out[column*4+2]=Number(left[2])*r0+Number(left[6])*r1+Number(left[10])*r2+Number(left[14])*r3;
    out[column*4+3]=Number(left[3])*r0+Number(left[7])*r1+Number(left[11])*r2+Number(left[15])*r3;
  }
  return out;
}

export function projectProjectPointToNormalized(
  point: ProjectPoint,
  projectionMatrix: ArrayLike<number>,
  worldFromView: ArrayLike<number>,
): NormalizedProjection | null {
  const viewFromWorld = rigidInverse(worldFromView);
  if (viewFromWorld === null) return null;
  const vp = multiplyMat4(projectionMatrix, viewFromWorld);
  if (vp === null) return null;

  // Project convention [X horizontal, Y horizontal, Z height] -> WebXR [X, Y-up, Z-back].
  const x=point[0]; const y=point[2]; const z=-point[1];
  const viewZ = viewFromWorld[2]*x+viewFromWorld[6]*y+viewFromWorld[10]*z+viewFromWorld[14];
  const viewDepthMeters = -viewZ;
  const clipX=vp[0]*x+vp[4]*y+vp[8]*z+vp[12];
  const clipY=vp[1]*x+vp[5]*y+vp[9]*z+vp[13];
  const clipZ=vp[2]*x+vp[6]*y+vp[10]*z+vp[14];
  const clipW=vp[3]*x+vp[7]*y+vp[11]*z+vp[15];
  if (![clipX,clipY,clipZ,clipW,viewDepthMeters].every(Number.isFinite) || clipW <= 1e-6 || viewDepthMeters <= 0) return null;
  const ndcX=clipX/clipW; const ndcY=clipY/clipW; const ndcZ=clipZ/clipW;
  const u=(ndcX+1)*0.5; const v=(1-ndcY)*0.5;
  return {
    u,
    v,
    clipDepth: ndcZ,
    viewDepthMeters,
    visible: u>=0&&u<=1&&v>=0&&v<=1&&ndcZ>=-1&&ndcZ<=1,
  };
}
