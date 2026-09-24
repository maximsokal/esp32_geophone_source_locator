import { Canvas, useFrame, useThree } from '@react-three/fiber';
import { useCallback, useEffect, useMemo, useState } from 'react';
import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';

import { clientDiagnostics } from '../data/clientDiagnostics';
import { describeMeshQuality } from '../xr/meshQualityUi';
import {
  displayFingerprint,
  scanModeLabel,
  type ScanDisplayOptions,
  type ScanMode,
} from '../xr/scanDisplayModes';
import {
  cameraPoseForPreset,
  clamp,
  orbitDistanceLimits,
  prepareScanScene,
  scanDimensionsText,
  scanLayerPlan,
  scanSourceBadgeText,
  toViewportPositions,
  type PreparedScanScene,
  type ScanLayerSpec,
} from '../xr/scanViewportScene';
import { SparseTsdfMeshClient } from '../xr/sparseTsdfClient';
import type { SparseTsdfMesh } from '../xr/sparseTsdf';
import { MESH_MIN_POINTS } from '../xr/surfaceComponents';
import type { ScanPointCloudSnapshot } from '../xr/webxrRuntime';
import type { ViewCommand } from './SceneViewport';

interface ScanPointCloudViewportProps {
  snapshot: ScanPointCloudSnapshot;
  viewCommand: ViewCommand;
  display: ScanDisplayOptions;
  mode: ScanMode | null;
}

function PointLayer({ positions, color, size, opacity, depthTest = true, renderOrder = 0 }: { positions: Float32Array; color: string; size: number; opacity: number; depthTest?: boolean; renderOrder?: number }) {
  const geometry = useMemo(() => { const next = new THREE.BufferGeometry(); next.setAttribute('position', new THREE.BufferAttribute(positions, 3)); return next; }, [positions]);
  useEffect(() => () => geometry.dispose(), [geometry]);
  if (positions.length === 0) return null;
  return <points geometry={geometry} frustumCulled={false} renderOrder={renderOrder}><pointsMaterial color={color} size={size} sizeAttenuation transparent={opacity < 1} opacity={opacity} depthTest={depthTest} depthWrite={false} /></points>;
}

function SurfaceMesh({ positions, wireframe }: { positions: Float32Array; wireframe: boolean }) {
  const geometry = useMemo(() => { const next = new THREE.BufferGeometry(); next.setAttribute('position', new THREE.BufferAttribute(positions, 3)); next.computeVertexNormals(); return next; }, [positions]);
  useEffect(() => () => geometry.dispose(), [geometry]); if (positions.length === 0) return null;
  return <group><mesh geometry={geometry} renderOrder={4}><meshBasicMaterial color="#38cfd2" transparent opacity={wireframe ? 0.30 : 0.22} side={THREE.DoubleSide} depthWrite={false} /></mesh>{wireframe && <mesh geometry={geometry} renderOrder={5}><meshBasicMaterial color="#a5f3fc" transparent opacity={0.22} wireframe side={THREE.DoubleSide} depthWrite={false} /></mesh>}</group>;
}

function LineLayer({ positions, color, opacity, renderOrder }: { positions: Float32Array; color: string; opacity: number; renderOrder: number }) {
  const geometry = useMemo(() => { const next = new THREE.BufferGeometry(); next.setAttribute('position', new THREE.BufferAttribute(positions, 3)); return next; }, [positions]);
  useEffect(() => () => geometry.dispose(), [geometry]); if (positions.length === 0) return null;
  return <lineSegments geometry={geometry} renderOrder={renderOrder}><lineBasicMaterial color={color} transparent opacity={opacity} depthTest={false} /></lineSegments>;
}

function ManualPin({ position, extent, id }: { position: readonly [number, number, number]; extent: number; id: number }) {
  const radius = clamp(extent * 0.022, 0.035, 0.10);
  return <group position={[position[0], position[1], position[2]]} renderOrder={50}><mesh renderOrder={50}><sphereGeometry args={[radius,18,12]} /><meshBasicMaterial color="#f05cff" depthTest={false} /></mesh><mesh rotation={[Math.PI/2,0,0]} renderOrder={51}><torusGeometry args={[radius*1.8,radius*0.18,8,28]} /><meshBasicMaterial color="#ffffff" depthTest={false} /></mesh><sprite position={[0,radius*3.1,0]} scale={[radius*4.5,radius*2,1]} renderOrder={52}><spriteMaterial color="#f05cff" depthTest={false} /></sprite><mesh position={[0,radius*3.1,0]} visible={false} name={`manual-pin-${id}`} /></group>;
}

function CameraController({ command, extent, onInteractionEnd }: { command: ViewCommand; extent: number; onInteractionEnd: (distance: number) => void }) {
  const { camera, gl } = useThree();
  const controls = useMemo(() => new OrbitControls(camera, gl.domElement), [camera, gl.domElement]);
  useEffect(() => {
    const limits = orbitDistanceLimits(extent);
    controls.enableDamping = true; controls.dampingFactor = 0.08; controls.enableRotate = true; controls.enableZoom = true; controls.enablePan = true; controls.screenSpacePanning = true; controls.rotateSpeed = 0.72; controls.zoomSpeed = 0.92; controls.panSpeed = 0.72; controls.minDistance = limits.minDistance; controls.maxDistance = limits.maxDistance; controls.touches.ONE = THREE.TOUCH.ROTATE; controls.touches.TWO = THREE.TOUCH.DOLLY_PAN; controls.target.set(0,0,0); controls.update();
    const handleStart = (): void => clientDiagnostics.info('SCAN_VIEW_GESTURE_START');
    const handleEnd = (): void => { const distance = controls.getDistance(); onInteractionEnd(distance); clientDiagnostics.info('SCAN_VIEW_GESTURE_END', { distance: Number(distance.toFixed(3)), x: Number(camera.position.x.toFixed(3)), y: Number(camera.position.y.toFixed(3)), z: Number(camera.position.z.toFixed(3)) }); };
    controls.addEventListener('start', handleStart); controls.addEventListener('end', handleEnd);
    return () => { controls.removeEventListener('start', handleStart); controls.removeEventListener('end', handleEnd); controls.dispose(); };
  }, [camera, controls, extent, onInteractionEnd]);
  useEffect(() => {
    const pose = cameraPoseForPreset(command.preset, extent);
    controls.target.set(pose.target[0], pose.target[1], pose.target[2]);
    camera.up.set(pose.up[0], pose.up[1], pose.up[2]);
    camera.position.set(pose.position[0], pose.position[1], pose.position[2]);
    camera.lookAt(controls.target); if (camera instanceof THREE.PerspectiveCamera) camera.updateProjectionMatrix(); controls.update(); onInteractionEnd(controls.getDistance());
  }, [camera, command.preset, command.sequence, controls, extent, onInteractionEnd]);
  useFrame(() => controls.update()); return null;
}

function LayerView({ layer, extent }: { layer: ScanLayerSpec; extent: number }) {
  switch (layer.kind) {
    case 'points': return <PointLayer positions={layer.positions} color={layer.color} size={layer.size} opacity={layer.opacity} depthTest={layer.depthTest} renderOrder={layer.renderOrder} />;
    case 'mesh': return <SurfaceMesh positions={layer.positions} wireframe={layer.wireframe} />;
    case 'lines': return <LineLayer positions={layer.positions} color={layer.color} opacity={layer.opacity} renderOrder={layer.renderOrder} />;
    case 'pins': return <>{layer.pins.map((position, index) => <ManualPin key={index} id={index + 1} position={position} extent={extent} />)}</>;
  }
}

function SceneLayers({ prepared, display, meshPositions, wireframe }: { prepared: PreparedScanScene; display: ScanDisplayOptions; meshPositions: Float32Array; wireframe: boolean }) {
  const layers = useMemo(() => scanLayerPlan(prepared, display, meshPositions, wireframe), [prepared, display, meshPositions, wireframe]);
  return <><color attach="background" args={['#07101a']} />{layers.map((layer) => <LayerView key={layer.id} layer={layer} extent={prepared.extent} />)}</>;
}

export function ScanPointCloudViewport({ snapshot, viewCommand, display, mode }: ScanPointCloudViewportProps) {
  const prepared = useMemo(() => prepareScanScene(snapshot), [snapshot]);
  const tsdfClient = useMemo(() => new SparseTsdfMeshClient(), []);
  const [tsdfMesh, setTsdfMesh] = useState<SparseTsdfMesh | null>(null);
  const [tsdfError, setTsdfError] = useState<string | null>(null);
  const [interactionRevision,setInteractionRevision] = useState(0);
  const [cameraDistance,setCameraDistance] = useState(Math.max(1.5, prepared.extent*1.75));

  useEffect(() => () => tsdfClient.dispose(), [tsdfClient]);
  useEffect(() => {
    let cancelled = false;
    tsdfClient.reset();
    setTsdfMesh(null);
    setTsdfError(null);
    if (snapshot.tsdfPointCount < MESH_MIN_POINTS) return () => { cancelled = true; };

    const started = performance.now();
    const maxTriangles = Math.max(3000, Math.min(18000, snapshot.tsdfPointCount * 14));
    void tsdfClient.reconstruct({
      // Use ONLY locally certified geometry for the implicit surface. Display
      // surfels remain unchanged, so the operator can still see where evidence
      // exists even when a region is deliberately excluded from the mesh.
      positions: snapshot.tsdfPositions.slice(),
      normals: snapshot.tsdfNormals.slice(),
      confidence: snapshot.tsdfCoverageConfidence.slice(),
      // A slightly coarser field than the 4-cm association grid suppresses
      // sub-voxel ripples that are unsupported at this depth resolution.
      voxelSizeMeters: snapshot.voxelSizeMeters * 1.20,
      freeSpacePositions: snapshot.freeAirPositions.slice(),
      freeSpaceVoxelSizeMeters: snapshot.occupancyVoxelSizeMeters,
      maxInputPoints: Math.min(9000, Math.max(1000, snapshot.tsdfPointCount)),
      maxTriangles,
    }).then((result) => {
      if (cancelled) return;
      setTsdfMesh(result);
      clientDiagnostics.info('SCAN_TSDF_READY', {
        sourcePoints: result.sourcePointCount,
        certifiedPoints: snapshot.tsdfPointCount,
        integratedPoints: result.integratedPointCount,
        nodes: result.nodeCount,
        rawTriangles: result.rawTriangleCount,
        triangles: result.triangleCount,
        quarantinedTriangles: result.quarantinedTriangleCount,
        localHotspots: result.localNoiseHotspotCount,
        localHotspotTriangles: result.localNoiseHotspotTriangleCount,
        discardedTriangles: result.topologyDiscardedTriangleCount,
        nonManifoldEdges: result.nonManifoldEdgeCount,
        orientationConflicts: result.orientationConflictCount,
        selfIntersections: result.selfIntersectionPairCount,
        selfIntersectionExactTests: result.selfIntersectionExactTestedPairs,
        selfIntersectionBroadPairs: result.selfIntersectionBroadPhaseCandidatePairs,
        selfIntersectionTruncated: result.selfIntersectionTruncated,
        meshNoiseScore: Number(result.meshNoiseScore.toFixed(3)),
        meshReadiness: Number(result.meshReadiness.toFixed(3)),
        voxelMm: Math.round(result.voxelSizeMeters * 1000),
        truncationMm: Math.round(result.truncationMeters * 1000),
        computeMs: Number(result.computeMs.toFixed(1)),
        roundTripMs: Number((performance.now() - started).toFixed(1)),
      });
      void clientDiagnostics.flush();
    }).catch((error) => {
      if (cancelled) return;
      const message = error instanceof Error ? error.message : String(error);
      if (message.includes('reset') || message.includes('superseded') || message.includes('disposed')) return;
      setTsdfError(message);
      clientDiagnostics.warn('SCAN_TSDF_FAILED', error);
    });
    return () => { cancelled = true; };
  }, [snapshot, tsdfClient]);

  const meshPositions = useMemo(
    () => tsdfMesh === null
      ? new Float32Array(0)
      : toViewportPositions(tsdfMesh.trianglePositions, prepared.centerX, prepared.centerY, prepared.centerZ),
    [prepared.centerX, prepared.centerY, prepared.centerZ, tsdfMesh],
  );

  const fingerprint = displayFingerprint(display); const modeLabel = scanModeLabel(mode);
  const handleInteractionEnd = useCallback((distance: number): void => { setCameraDistance(distance); setInteractionRevision((current) => current+1); }, []);
  const meshQuality = tsdfMesh === null ? null : describeMeshQuality(tsdfMesh);
  const meshStatus = snapshot.tsdfPointCount < MESH_MIN_POINTS
    ? 'MESH · SCAN MORE FOR CERTIFIED GEOMETRY'
    : tsdfError !== null
      ? 'MESH · UNAVAILABLE'
      : meshQuality === null
        ? 'MESH · BUILDING…'
        : `${meshQuality.label} · ${snapshot.tsdfPointCount.toLocaleString()} surfels`;

  return <div className="viewport-shell scan-viewport" aria-label="Measured WebXR rescue search volume" data-scan-mode={mode ?? 'custom'} data-layer-fingerprint={fingerprint} data-camera-revision={interactionRevision} data-camera-distance={cameraDistance.toFixed(4)}>
    <Canvas className="scan-webgl-canvas" camera={{ position:[3.2,2.5,3.6], fov:50, near:0.01, far:120 }} dpr={[1,1.5]} gl={{ antialias:true, alpha:false, powerPreference:'high-performance' }} onContextMenu={(event) => event.preventDefault()}><SceneLayers prepared={prepared} display={display} meshPositions={meshPositions} wireframe={mode === 'mesh'} /><CameraController command={viewCommand} extent={prepared.extent} onInteractionEnd={handleInteractionEnd} /></Canvas>
    <div className="scan-source-badge">{scanSourceBadgeText(snapshot)}</div>
    <div className="scan-mode-overlay">{modeLabel} · {fingerprint.toUpperCase()}</div>
    {display.dimensions && <div className="scan-dimension-overlay">{scanDimensionsText(prepared)}</div>}
    {display.mesh && <div className="scan-mesh-overlay" data-mesh-quality={meshQuality?.severity ?? 'pending'}>{meshStatus}</div>}
    <div className="axis-note">1 finger = orbit · 2 fingers = zoom/pan · X/Y horizontal · Z height</div>
  </div>;
}
