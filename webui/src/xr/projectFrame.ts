/** Converts a sensor-to-WebXR-local-floor transform into the project frame. */
export function localFloorMatrixToProject(matrix: readonly number[]): number[] {
  if (!Array.isArray(matrix) || matrix.length !== 16) {
    throw new Error('Registration matrix must contain 16 row-major values.');
  }
  if (matrix.some((value) => !Number.isFinite(value))) {
    throw new Error('Registration matrix values must be finite.');
  }

  // `0 - x` keeps zero entries at +0; plain negation would write -0 into the artifact bytes.
  return [
    matrix[0], matrix[1], matrix[2], matrix[3],
    0 - matrix[8], 0 - matrix[9], 0 - matrix[10], 0 - matrix[11],
    matrix[4], matrix[5], matrix[6], matrix[7],
    matrix[12], matrix[13], matrix[14], matrix[15],
  ];
}
