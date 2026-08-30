export type Vec3 = readonly [number, number, number];

export type SensorRole = 'main' | 'remote';

export interface SensorPlacement {
  id: string;
  label: string;
  role: SensorRole;
  position: Vec3;
  locked: boolean;
  uncertaintyM?: Vec3;
}

export interface TriangleMesh {
  vertices: Vec3[];
  indices: number[];
}

export interface SceneModel {
  frameId: string;
  units: 'm';
  source: 'demo' | 'scan' | 'saved';
  sensors: SensorPlacement[];
  surface?: TriangleMesh;
  capturedAt?: string;
}
