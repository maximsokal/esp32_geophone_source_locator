import type { SceneModel, TriangleMesh, Vec3 } from '../domain/scene';

function createDemoSurface(): TriangleMesh {
  const size = 7;
  const step = 1.35;
  const half = ((size - 1) * step) / 2;
  const vertices: Vec3[] = [];
  const indices: number[] = [];

  for (let row = 0; row < size; row += 1) {
    for (let column = 0; column < size; column += 1) {
      const x = column * step - half;
      const y = row * step - half;
      const radius = Math.hypot(x, y);
      const mound = Math.max(0, 1 - radius / 6.4) * 0.72;
      const roughness = Math.sin(x * 1.15) * Math.cos(y * 0.95) * 0.16;
      vertices.push([x, y, Math.max(0.05, mound + roughness)]);
    }
  }

  for (let row = 0; row < size - 1; row += 1) {
    for (let column = 0; column < size - 1; column += 1) {
      const a = row * size + column;
      const b = a + 1;
      const c = a + size;
      const d = c + 1;
      indices.push(a, c, b, b, c, d);
    }
  }

  return { vertices, indices };
}

export const demoScene: SceneModel = {
  frameId: 'MAIN_LOCAL',
  units: 'm',
  source: 'demo',
  surface: createDemoSurface(),
  sensors: [
    {
      id: 'MAIN',
      label: 'MAIN',
      role: 'main',
      position: [0, 0, 0],
      locked: true,
      uncertaintyM: [0.02, 0.02, 0.02],
    },
    {
      id: 'GEO1',
      label: 'G1',
      role: 'remote',
      position: [-3.2, 1.7, 0.31],
      locked: true,
      uncertaintyM: [0.12, 0.12, 0.08],
    },
    {
      id: 'GEO2',
      label: 'G2',
      role: 'remote',
      position: [3.45, 2.05, 0.48],
      locked: true,
      uncertaintyM: [0.14, 0.13, 0.09],
    },
    {
      id: 'GEO3',
      label: 'G3',
      role: 'remote',
      position: [-1.95, -3.25, 0.93],
      locked: true,
      uncertaintyM: [0.16, 0.16, 0.1],
    },
    {
      id: 'GEO4',
      label: 'G4',
      role: 'remote',
      position: [3.05, -2.85, 0.44],
      locked: true,
      uncertaintyM: [0.15, 0.14, 0.09],
    },
  ],
};
