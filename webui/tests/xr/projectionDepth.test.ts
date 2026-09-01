import { describe, expect, it } from 'vitest';

import { projectProjectPointToNormalized } from '../../src/xr/projectionMath';

const identity = Float32Array.from([
  1,0,0,0,
  0,1,0,0,
  0,0,1,0,
  0,0,0,1,
]);

function perspective(near = 0.1, far = 10): Float32Array {
  const a = (far + near) / (near - far);
  const b = (2 * far * near) / (near - far);
  return Float32Array.from([
    1,0,0,0,
    0,1,0,0,
    0,0,a,-1,
    0,0,b,0,
  ]);
}

describe('XR projection metric view depth', () => {
  it('preserves front-to-back metric ordering for overlapping screen points', () => {
    const projection = perspective();
    const near = projectProjectPointToNormalized([0, 1, 0], projection, identity);
    const far = projectProjectPointToNormalized([0, 2, 0], projection, identity);
    expect(near?.visible).toBe(true);
    expect(far?.visible).toBe(true);
    expect(near?.u).toBeCloseTo(far!.u, 6);
    expect(near?.v).toBeCloseTo(far!.v, 6);
    expect(near?.viewDepthMeters).toBeCloseTo(1, 6);
    expect(far?.viewDepthMeters).toBeCloseTo(2, 6);
    expect(near!.viewDepthMeters!).toBeLessThan(far!.viewDepthMeters!);
  });
});
