import { Canvas, useFrame, useThree } from '@react-three/fiber';
import { useCallback, useEffect, useMemo, useState } from 'react';
import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';

import { clientDiagnostics } from '../data/clientDiagnostics';
import { buildSurfaceMeshPreview } from '../xr/meshPreview';
import {
  displayFingerprint,
  scanModeLabel,
  type ScanDisplayOptions,
  type ScanMode,
} from '../xr/scanDisplayModes';
import type { ScanPointCloudSnapshot } from '../xr/webxrRuntime';
import type { ViewCommand } from './SceneViewport';

interface ScanPointCloudViewportProps {
  snapshot: ScanPointCloudSnapshot;
  viewCommand: ViewCommand;
  display: ScanDisplayOptions;
  mode: ScanMode | null;
}

interface QualityBands {
  weak: Float32Array;
  improving: Float32Array;
  stable: Float32Array;
}

interface PreparedScene {
  good: QualityBands;
  bad: QualityBands;
  manualPositions: ReadonlyArray<readonly [number, number, number]>;
  meshPositions: Float32Array;
  freeAirPositions: Float32Array;
  unknownPositions: Float32Array;
  boundsLines: Float32Array;
  extent: number;
  extentX: number;
  extentY: number;
  extentZ: number;
  meshTriangles: number;
}

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}

function transformOne(x: number, y: number, z: number, centerX: number, centerY: number, centerZ: number): readonly [number, number, number] {
  return [x - centerX, z - centerZ, -(y - centerY)];
}

function transformPositions(source: Float32Array, centerX: number, centerY: number, centerZ: number): Float32Array {
  const output = new Float32Array(source.length);
  for (let index = 0; index < source.length; index += 3) {
    const transformed = transformOne(source[index], source[index + 1], source[index + 2], centerX, centerY, centerZ);
    output[index] = transformed[0]; output[index + 1] = transformed[1]; output[index + 2] = transformed[2];
  }
  return output;
}

function splitConfidenceBands(positions: Float32Array, confidence: Float32Array, centerX: number, centerY: number, centerZ: number): QualityBands {
  const weak: number[] = []; const improving: number[] = []; const stable: number[] = [];
  const count = Math.min(Math.floor(positions.length / 3), confidence.length);
  for (let index = 0; index < count; index += 1) {
    const offset = index * 3;
    const transformed = transformOne(positions[offset], positions[offset + 1], positions[offset + 2], centerX, centerY, centerZ);
    const target = confidence[index] < 0.42 ? weak : confidence[index] < 0.72 ? improving : stable;
    target.push(transformed[0], transformed[1], transformed[2]);
  }
  return { weak: Float32Array.from(weak), improving: Float32Array.from(improving), stable: Float32Array.from(stable) };
}

function boundsLinePositions(snapshot: ScanPointCloudSnapshot, centerX: number, centerY: number, centerZ: number): Float32Array {
  const bounds = snapshot.bounds;
  if (bounds === null) return new Float32Array(0);
  const corners: Array<readonly [number, number, number]> = [];
  for (const x of [bounds.min[0], bounds.max[0]]) for (const y of [bounds.min[1], bounds.max[1]]) for (const z of [bounds.min[2], bounds.max[2]]) corners.push(transformOne(x, y, z, centerX, centerY, centerZ));
  const edges: readonly [number, number][] = [[0,1],[0,2],[0,4],[1,3],[1,5],[2,3],[2,6],[3,7],[4,5],[4,6],[5,7],[6,7]];
  const output = new Float32Array(edges.length * 6); let write = 0;
  for (const [a,b] of edges) for (const pointIndex of [a,b]) { const point = corners[pointIndex]; output[write++] = point[0]; output[write++] = point[1]; output[write++] = point[2]; }
  return output;
}

function prepareScene(snapshot: ScanPointCloudSnapshot): PreparedScene {
  const bounds = snapshot.bounds;
  const centerX = bounds === null ? 0 : (bounds.min[0] + bounds.max[0]) * 0.5;
  const centerY = bounds === null ? 0 : (bounds.min[1] + bounds.max[1]) * 0.5;
  const centerZ = bounds === null ? 0 : (bounds.min[2] + bounds.max[2]) * 0.5;
  const extentX = bounds === null ? 0 : bounds.max[0] - bounds.min[0];
  const extentY = bounds === null ? 0 : bounds.max[1] - bounds.min[1];
  const extentZ = bounds === null ? 0 : bounds.max[2] - bounds.min[2];
  const extent = Math.max(0.8, extentX, extentY, extentZ);
  const mesh = buildSurfaceMeshPreview({ positions: snapshot.positions, normals: snapshot.normals, confidence: snapshot.confidence, voxelSizeMeters: snapshot.voxelSizeMeters });
  return {
    good: splitConfidenceBands(snapshot.positions, snapshot.confidence, centerX, centerY, centerZ),
    bad: splitConfidenceBands(snapshot.tentativePositions, snapshot.tentativeConfidence, centerX, centerY, centerZ),
    manualPositions: snapshot.manualControlPoints.map((point) => transformOne(point.position[0], point.position[1], point.position[2], centerX, centerY, centerZ)),
    meshPositions: transformPositions(mesh.trianglePositions, centerX, centerY, centerZ),
    freeAirPositions: transformPositions(snapshot.freeAirPositions, centerX, centerY, centerZ),
    unknownPositions: transformPositions(snapshot.unknownCandidatePositions, centerX, centerY, centerZ),
    boundsLines: boundsLinePositions(snapshot, centerX, centerY, centerZ), extent, extentX, extentY, extentZ, meshTriangles: mesh.triangleCount,
  };
}

function PointLayer({ positions, color, size, opacity, depthTest = true, renderOrder = 0 }: { positions: Float32Array; color: string; size: number; opacity: number; depthTest?: boolean; renderOrder?: number }) {
  const geometry = useMemo(() => { const next = new THREE.BufferGeometry(); next.setAttribute('position', new THREE.BufferAttribute(positions, 3)); return next; }, [positions]);
  useEffect(() => () => geometry.dispose(), [geometry]);
  if (positions.length === 0) return null;
  return <points geometry={geometry} frustumCulled={false} renderOrder={renderOrder}><pointsMaterial color={color} size={size} sizeAttenuation transparent={opacity < 1} opacity={opacity} depthTest={depthTest} depthWrite={false} /></points>;
}

function QualityPointLayers({ bands, extent, faint = false }: { bands: QualityBands; extent: number; faint?: boolean }) {
  const base = clamp(extent * 0.008, 0.014, 0.045);
  return <><PointLayer positions={bands.weak} color="#ff3d28" size={base * 0.86} opacity={faint ? 0.42 : 0.62} /><PointLayer positions={bands.improving} color="#facc15" size={base} opacity={faint ? 0.58 : 0.82} /><PointLayer positions={bands.stable} color="#2ee6a6" size={base * 1.12} opacity={faint ? 0.76 : 0.98} /></>;
}

function SurfaceMesh({ positions }: { positions: Float32Array }) {
  const geometry = useMemo(() => { const next = new THREE.BufferGeometry(); next.setAttribute('position', new THREE.BufferAttribute(positions, 3)); next.computeVertexNormals(); return next; }, [positions]);
  useEffect(() => () => geometry.dispose(), [geometry]); if (positions.length === 0) return null;
  return <group><mesh geometry={geometry} renderOrder={4}><meshBasicMaterial color="#38cfd2" transparent opacity={0.34} side={THREE.DoubleSide} depthWrite={false} /></mesh><mesh geometry={geometry} renderOrder={5}><meshBasicMaterial color="#a5f3fc" transparent opacity={0.34} wireframe side={THREE.DoubleSide} depthWrite={false} /></mesh></group>;
}

function BoundsLines({ positions }: { positions: Float32Array }) {
  const geometry = useMemo(() => { const next = new THREE.BufferGeometry(); next.setAttribute('position', new THREE.BufferAttribute(positions, 3)); return next; }, [positions]);
  useEffect(() => () => geometry.dispose(), [geometry]); if (positions.length === 0) return null;
  return <lineSegments geometry={geometry} renderOrder={20}><lineBasicMaterial color="#dbeafe" transparent opacity={0.78} depthTest={false} /></lineSegments>;
}

function ManualPin({ position, extent, id }: { position: readonly [number, number, number]; extent: number; id: number }) {
  const radius = clamp(extent * 0.022, 0.035, 0.10);
  return <group position={[position[0], position[1], position[2]]} renderOrder={50}><mesh renderOrder={50}><sphereGeometry args={[radius,18,12]} /><meshBasicMaterial color="#f05cff" depthTest={false} /></mesh><mesh rotation={[Math.PI/2,0,0]} renderOrder={51}><torusGeometry args={[radius*1.8,radius*0.18,8,28]} /><meshBasicMaterial color="#ffffff" depthTest={false} /></mesh><sprite position={[0,radius*3.1,0]} scale={[radius*4.5,radius*2,1]} renderOrder={52}><spriteMaterial color="#f05cff" depthTest={false} /></sprite><mesh position={[0,radius*3.1,0]} visible={false} name={`manual-pin-${id}`} /></group>;
}

function CameraController({ command, extent, onInteractionEnd }: { command: ViewCommand; extent: number; onInteractionEnd: (distance: number) => void }) {
  const { camera, gl } = useThree();
  const controls = useMemo(() => new OrbitControls(camera, gl.domElement), [camera, gl.domElement]);
  useEffect(() => {
    controls.enableDamping = true; controls.dampingFactor = 0.08; controls.enableRotate = true; controls.enableZoom = true; controls.enablePan = true; controls.screenSpacePanning = true; controls.rotateSpeed = 0.72; controls.zoomSpeed = 0.92; controls.panSpeed = 0.72; controls.minDistance = Math.max(0.18, extent*0.10); controls.maxDistance = Math.max(18, extent*10); controls.touches.ONE = THREE.TOUCH.ROTATE; controls.touches.TWO = THREE.TOUCH.DOLLY_PAN; controls.target.set(0,0,0); controls.update();
    const handleStart = (): void => clientDiagnostics.info('SCAN_VIEW_GESTURE_START');
    const handleEnd = (): void => { const distance = controls.getDistance(); onInteractionEnd(distance); clientDiagnostics.info('SCAN_VIEW_GESTURE_END', { distance: Number(distance.toFixed(3)), x: Number(camera.position.x.toFixed(3)), y: Number(camera.position.y.toFixed(3)), z: Number(camera.position.z.toFixed(3)) }); };
    controls.addEventListener('start', handleStart); controls.addEventListener('end', handleEnd);
    return () => { controls.removeEventListener('start', handleStart); controls.removeEventListener('end', handleEnd); controls.dispose(); };
  }, [camera, controls, extent, onInteractionEnd]);
  useEffect(() => {
    const distance = Math.max(1.5, extent * 1.75); controls.target.set(0,0,0);
    if (command.preset === 'top') { camera.up.set(0,0,-1); camera.position.set(0,distance,0.001); }
    else if (command.preset === 'front') { camera.up.set(0,1,0); camera.position.set(0,extent*0.08,distance); }
    else { camera.up.set(0,1,0); camera.position.set(distance*0.66,distance*0.52,distance*0.72); }
    camera.lookAt(controls.target); if (camera instanceof THREE.PerspectiveCamera) camera.updateProjectionMatrix(); controls.update(); onInteractionEnd(controls.getDistance());
  }, [camera, command.preset, command.sequence, controls, extent, onInteractionEnd]);
  useFrame(() => controls.update()); return null;
}

function SceneLayers({ prepared, display }: { prepared: PreparedScene; display: ScanDisplayOptions }) {
  const voxelPointSize = clamp(prepared.extent*0.010,0.018,0.055);
  return <><color attach="background" args={['#07101a']} />{display.unknownSpace && <PointLayer positions={prepared.unknownPositions} color="#f59e0b" size={voxelPointSize} opacity={0.30} depthTest={false} renderOrder={1} />}{display.freeAir && <PointLayer positions={prepared.freeAirPositions} color="#38bdf8" size={voxelPointSize*0.82} opacity={0.48} depthTest={false} renderOrder={2} />}{display.mesh && <SurfaceMesh positions={prepared.meshPositions} />}{display.badPoints && <QualityPointLayers bands={prepared.bad} extent={prepared.extent} faint />}{display.goodPoints && <QualityPointLayers bands={prepared.good} extent={prepared.extent} />}{display.manualPoints && prepared.manualPositions.map((position,index) => <ManualPin key={index} id={index+1} position={position} extent={prepared.extent} />)}{display.dimensions && <BoundsLines positions={prepared.boundsLines} />}</>;
}

export function ScanPointCloudViewport({ snapshot, viewCommand, display, mode }: ScanPointCloudViewportProps) {
  const prepared = useMemo(() => prepareScene(snapshot), [snapshot]);
  const [interactionRevision,setInteractionRevision] = useState(0);
  const [cameraDistance,setCameraDistance] = useState(Math.max(1.5, prepared.extent*1.75));
  const fingerprint = displayFingerprint(display); const modeLabel = scanModeLabel(mode);
  const handleInteractionEnd = useCallback((distance: number): void => { setCameraDistance(distance); setInteractionRevision((current) => current+1); }, []);
  return <div className="viewport-shell scan-viewport" aria-label="Measured WebXR rescue search volume" data-scan-mode={mode ?? 'custom'} data-layer-fingerprint={fingerprint} data-camera-revision={interactionRevision} data-camera-distance={cameraDistance.toFixed(4)}>
    <Canvas className="scan-webgl-canvas" camera={{ position:[3.2,2.5,3.6], fov:50, near:0.01, far:120 }} dpr={[1,1.5]} gl={{ antialias:true, alpha:false, powerPreference:'high-performance' }} onContextMenu={(event) => event.preventDefault()}><SceneLayers prepared={prepared} display={display} /><CameraController command={viewCommand} extent={prepared.extent} onInteractionEnd={handleInteractionEnd} /></Canvas>
    <div className="scan-source-badge">SURFACE {snapshot.pointCount.toLocaleString()} · FREE {snapshot.knownFreeVoxelCount.toLocaleString()} · UNKNOWN {snapshot.unknownCandidateVoxelCount.toLocaleString()} · PINS {snapshot.manualControlPoints.length.toLocaleString()}</div>
    <div className="scan-mode-overlay">{modeLabel} · {fingerprint.toUpperCase()}</div>
    {display.dimensions && <div className="scan-dimension-overlay">X {prepared.extentX.toFixed(2)} m · Y {prepared.extentY.toFixed(2)} m · Z {prepared.extentZ.toFixed(2)} m</div>}
    {display.mesh && <div className="scan-mesh-overlay">SURFACE BOUNDARY · {prepared.meshTriangles.toLocaleString()} triangles</div>}
    <div className="axis-note">1 finger = orbit · 2 fingers = zoom/pan · X/Y horizontal · Z height</div>
  </div>;
}
