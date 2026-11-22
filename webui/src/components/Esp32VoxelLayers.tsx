import { useEffect, useMemo } from 'react';
import * as THREE from 'three';

import { uiTheme } from '../theme';
import {
  buildEsp32VoxelGeometry,
  decodeVoxelCell,
  type Esp32VoxelGeometry,
  type VoxelCell,
  type VoxelSceneCentre,
} from '../xr/esp32VoxelScene';
import { buildSolverVoxelGeometry, solverVoxelCell, type SolverVoxelCell } from '../xr/esp32OccupancyScene';
import type { DecodedOccupancyArtifact } from '../xr/occupancySolverArtifact';
import type { VoxelPreviewArtifact } from '../xr/voxelPreview';

/** What the ESP32 receives or has received, shown in place of the scan layers (issue #44). The data is decoded from the exact bytes. */
export type Esp32VoxelStatus = 'prepared' | 'uploading' | 'saved' | 'failed' | 'stale' | 'unavailable';
export type Esp32VoxelView =
  | { kind: 'preview'; artifact: VoxelPreviewArtifact; status: Esp32VoxelStatus }
  | { kind: 'solver'; occupancy: DecodedOccupancyArtifact; status: Esp32VoxelStatus };

export type VoxelPick =
  | { kind: 'preview'; cell: VoxelCell }
  | { kind: 'solver'; cell: SolverVoxelCell };

/** The decoded source a view shows; its identity changes exactly when the voxels change. */
export function esp32VoxelSource(view: Esp32VoxelView | null): VoxelPreviewArtifact | DecodedOccupancyArtifact | null {
  return view === null ? null : view.kind === 'preview' ? view.artifact : view.occupancy;
}

const shadeByNormal = (normalX: number, normalY: number): number => normalY > 0.5 ? 1 : normalY < -0.5 ? 0.45 : Math.abs(normalX) > 0.5 ? 0.78 : 0.62;

/** Per-vertex colour = base colour x a fixed shade by face direction: the layer looks the same under any scene lighting. */
function voxelColors(geometry: Esp32VoxelGeometry, color: string): Float32Array {
  const colors = new Float32Array(geometry.positions.length);
  const base = new THREE.Color(color);
  for (let vertex = 0; vertex < colors.length / 3; vertex += 1) {
    const shade = shadeByNormal(geometry.normals[vertex * 3], geometry.normals[vertex * 3 + 1]);
    colors[vertex * 3] = base.r * shade; colors[vertex * 3 + 1] = base.g * shade; colors[vertex * 3 + 2] = base.b * shade;
  }
  return colors;
}

function useBatchedGeometry(built: Esp32VoxelGeometry, color: string): THREE.BufferGeometry {
  const geometry = useMemo(() => {
    const next = new THREE.BufferGeometry();
    next.setAttribute('position', new THREE.BufferAttribute(built.positions, 3));
    next.setAttribute('normal', new THREE.BufferAttribute(built.normals, 3));
    next.setAttribute('color', new THREE.BufferAttribute(voxelColors(built, color), 3));
    next.computeBoundingSphere();
    return next;
  }, [built, color]);
  useEffect(() => () => geometry.dispose(), [geometry]);
  return geometry;
}

/** The transmitted GXPRV1 cells as one batched geometry: exposed faces only, one buffer, picking by triangle. */
function PreviewVoxelMesh({ artifact, centre, onPick }: { artifact: VoxelPreviewArtifact; centre: VoxelSceneCentre; onPick: (pick: VoxelPick | null) => void }) {
  const built = useMemo(() => buildEsp32VoxelGeometry(artifact, centre), [artifact, centre.x, centre.y, centre.z]);
  const geometry = useBatchedGeometry(built, uiTheme.accent);
  if (built.triangleCount === 0) return null;
  return <mesh geometry={geometry} renderOrder={3} onClick={(event) => {
    event.stopPropagation();
    const triangle = event.faceIndex;
    onPick(triangle === undefined || triangle === null ? null : { kind: 'preview', cell: decodeVoxelCell(artifact, built.triangleCell[triangle]) });
  }}><meshBasicMaterial vertexColors transparent opacity={0.92} side={THREE.FrontSide} /></mesh>;
}

/** The solver's OCCUPIED cells (solid) and FREE cells (faint, so what is behind stays readable); UNKNOWN is never drawn. */
function SolverVoxelMesh({ occupancy, centre, onPick }: { occupancy: DecodedOccupancyArtifact; centre: VoxelSceneCentre; onPick: (pick: VoxelPick | null) => void }) {
  const built = useMemo(() => buildSolverVoxelGeometry(occupancy, centre), [occupancy, centre.x, centre.y, centre.z]);
  const occupiedGeometry = useBatchedGeometry(built.occupied, uiTheme.accent);
  const freeGeometry = useBatchedGeometry(built.free, '#5aa9ff');
  const pick = (cls: 'occupied' | 'free', geometry: Esp32VoxelGeometry) => (event: { stopPropagation: () => void; faceIndex?: number | null }) => {
    event.stopPropagation();
    const triangle = event.faceIndex;
    onPick(triangle === undefined || triangle === null ? null : { kind: 'solver', cell: solverVoxelCell(built, cls, geometry.triangleCell[triangle]) });
  };
  return <>
    {built.occupied.triangleCount > 0 && <mesh geometry={occupiedGeometry} renderOrder={3} onClick={pick('occupied', built.occupied)}>
      <meshBasicMaterial vertexColors transparent opacity={0.92} side={THREE.FrontSide} /></mesh>}
    {built.free.triangleCount > 0 && <mesh geometry={freeGeometry} renderOrder={2} onClick={pick('free', built.free)}>
      <meshBasicMaterial vertexColors transparent opacity={0.28} side={THREE.FrontSide} depthWrite={false} /></mesh>}
  </>;
}

export function Esp32VoxelScene({ view, centre, onPick }: { view: Esp32VoxelView; centre: VoxelSceneCentre; onPick: (pick: VoxelPick | null) => void }) {
  return view.kind === 'preview'
    ? <PreviewVoxelMesh artifact={view.artifact} centre={centre} onPick={onPick} />
    : <SolverVoxelMesh occupancy={view.occupancy} centre={centre} onPick={onPick} />;
}
